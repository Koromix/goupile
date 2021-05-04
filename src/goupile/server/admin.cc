// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "../../core/libcc/libcc.hh"
#include "admin.hh"
#include "domain.hh"
#include "files.hh"
#include "goupile.hh"
#include "instance.hh"
#include "session.hh"
#include "../../core/libwrap/json.hh"
#include "../../../vendor/libsodium/src/libsodium/include/sodium.h"
#include "../../../vendor/miniz/miniz.h"

#include <time.h>
#ifdef _WIN32
    #include <io.h>

    typedef unsigned int uid_t;
    typedef unsigned int gid_t;
#else
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace RG {

static const char *DefaultConfig =
R"([Paths]
# DatabaseFile = goupile.db
# InstanceDirectory = instances
# TempDirectory = tmp
# BackupDirectory = backup

[Data]
BackupKey = %1
# SynchronousFull = Off

[SMS]
# Provider = Twilio
# AuthID = <AuthID>
# AuthToken = <AuthToken>
# From = <Phone number or alphanumeric sender>

[SMTP]
# URL = <Curl URL>
# Username = <Username> (if any)
# Password = <Password> (if any)
# From = <Sender email address>

[HTTP]
# SocketType = Dual
# Port = 8888
# Threads =
# AsyncThreads =
# TrustXRealIP = Off
)";

#pragma pack(push, 1)
struct ArchiveIntro {
    char signature[15];
    int8_t version;
    uint8_t eskey[crypto_secretstream_xchacha20poly1305_KEYBYTES + crypto_box_SEALBYTES];
    uint8_t header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
};
#pragma pack(pop)
#define ARCHIVE_VERSION 1
#define ARCHIVE_SIGNATURE "GOUPILE_BACKUP"

static bool CheckInstanceKey(Span<const char> key)
{
    const auto test_char = [](char c) { return (c >= 'a' && c <= 'z') || IsAsciiDigit(c) || c == '_'; };

    // Skip master prefix
    bool slave;
    {
        Span<const char> master = SplitStr(key, '/', &key);

        if (key.ptr > master.end()) {
            slave = true;
        } else {
            key = master;
            slave = false;
        }
    }

    if (!key.len) {
        LogError("Instance key cannot be empty");
        return false;
    }
    if (key.len > 64) {
        LogError("Instance key cannot have more than 64 characters");
        return false;
    }
    if (!std::all_of(key.begin(), key.end(), test_char)) {
        LogError("Instance key must only contain lowercase alphanumeric or '_' characters");
        return false;
    }

    // Reserved names
    if (slave) {
        if (key == "main" || key == "static" || key == "files") {
            LogError("The following slave keys are not allowed: main, static, files");
            return false;
        }
    } else {
        if (key == "admin") {
            LogError("The following instance keys are not allowed: admin");
            return false;
        }
    }

    return true;
}

static bool CheckUserName(Span<const char> username)
{
    const auto test_char = [](char c) { return (c >= 'a' && c <= 'z') || IsAsciiDigit(c) || c == '_' || c == '.' || c == '-'; };

    if (!username.len) {
        LogError("Username cannot be empty");
        return false;
    }
    if (username.len > 64) {
        LogError("Username cannot be have more than 64 characters");
        return false;
    }
    if (!std::all_of(username.begin(), username.end(), test_char)) {
        LogError("Username must only contain lowercase alphanumeric, '_', '.' or '-' characters");
        return false;
    }

    return true;
}

#ifndef _WIN32
static bool FindPosixUser(const char *username, uid_t *out_uid, gid_t *out_gid)
{
    struct passwd pwd_buf;
    HeapArray<char> buf;
    struct passwd *pwd;

again:
    buf.Grow(1024);
    buf.len += 1024;

    int ret = getpwnam_r(username, &pwd_buf, buf.ptr, buf.len, &pwd);
    if (ret != 0) {
        if (ret == ERANGE)
            goto again;

        LogError("getpwnam('%1') failed: %2", username, strerror(errno));
        return false;
    }
    if (!pwd) {
        LogError("Could not find system user '%1'", username);
        return false;
    }

    *out_uid = pwd->pw_uid;
    *out_gid = pwd->pw_gid;
    return true;
}
#endif

static bool HashPassword(Span<const char> password, char out_hash[crypto_pwhash_STRBYTES])
{
    if (crypto_pwhash_str(out_hash, password.ptr, password.len,
                          crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        LogError("Failed to hash password");
        return false;
    }

    return true;
}

static bool ChangeFileOwner(const char *filename, uid_t uid, gid_t gid)
{
#ifndef _WIN32
    if (chown(filename, uid, gid) < 0) {
        LogError("Failed to change '%1' owner: %2", filename, strerror(errno));
        return false;
    }
#endif

    return true;
}

static bool CreateInstance(DomainHolder *domain, const char *instance_key,
                           const char *title, int64_t default_userid, bool demo, int *out_error)
{
    BlockAllocator temp_alloc;

    *out_error = 500;

    // Check for existing instance
    {
        sq_Statement stmt;
        if (!domain->db.Prepare("SELECT instance FROM dom_instances WHERE instance = ?1", &stmt))
            return false;
        sqlite3_bind_text(stmt, 1, instance_key, -1, SQLITE_STATIC);

        if (stmt.Next()) {
            LogError("Instance '%1' already exists", instance_key);
            *out_error = 409;
            return false;
        } else if (!stmt.IsValid()) {
            return false;
        }
    }

    const char *database_filename = domain->config.GetInstanceFileName(instance_key, &temp_alloc);
    if (TestFile(database_filename)) {
        LogError("Database '%1' already exists (old deleted instance?)", database_filename);
        *out_error = 409;
        return false;
    }
    RG_DEFER_N(db_guard) { UnlinkFile(database_filename); };

    uid_t owner_uid = 0;
    gid_t owner_gid = 0;
#ifndef _WIN32
    {
        struct stat sb;
        if (stat(domain->config.database_filename, &sb) < 0) {
            LogError("Failed to stat '%1': %2", database_filename, strerror(errno));
            return false;
        }

        owner_uid = sb.st_uid;
        owner_gid = sb.st_gid;
    }
#endif

    // Create instance database
    sq_Database db;
    if (!db.Open(database_filename, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
        return false;
    if (!MigrateInstance(&db))
        return false;
    if (!ChangeFileOwner(database_filename, owner_uid, owner_gid))
        return false;

    // Set default settings
    {
        const char *sql = "UPDATE fs_settings SET value = ?2 WHERE key = ?1";
        bool success = true;

        success &= db.Run(sql, "Title", title);

        if (!success)
            return false;
    }

    // Create default files
    if (demo) {
        sq_Statement stmt;
        if (!db.Prepare(R"(INSERT INTO fs_files (active, filename, mtime, blob, compression, sha256, size)
                           VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6))", &stmt))
            return false;

        // Use same modification time for all files
        int64_t mtime = GetUnixTime();

        for (const AssetInfo &asset: GetPackedAssets()) {
            if (StartsWith(asset.name, "src/goupile/demo/")) {
                const char *filename = asset.name + 17;

                CompressionType compression_type = ShouldCompressFile(filename) ? CompressionType::Gzip
                                                                                : CompressionType::None;

                HeapArray<uint8_t> gzip;
                char sha256[65];
                Size total_len = 0;
                {
                    StreamReader reader(asset.data, "<asset>", asset.compression_type);
                    StreamWriter writer(&gzip, "<gzip>", compression_type);

                    crypto_hash_sha256_state state;
                    crypto_hash_sha256_init(&state);

                    while (!reader.IsEOF()) {
                        LocalArray<uint8_t, 16384> buf;
                        buf.len = reader.Read(buf.data);
                        if (buf.len < 0)
                            return false;
                        total_len += buf.len;

                        writer.Write(buf);
                        crypto_hash_sha256_update(&state, buf.data, buf.len);
                    }

                    bool success = writer.Close();
                    RG_ASSERT(success);

                    uint8_t hash[crypto_hash_sha256_BYTES];
                    crypto_hash_sha256_final(&state, hash);
                    FormatSha256(hash, sha256);
                }

                stmt.Reset();
                sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 2, mtime);
                sqlite3_bind_blob64(stmt, 3, gzip.ptr, gzip.len, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 4, CompressionTypeNames[(int)compression_type], -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 5, sha256, -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 6, total_len);

                if (!stmt.Run())
                    return false;
            }
        }
    }

    if (!db.Close())
        return false;

    bool success = domain->db.Transaction([&]() {
        if (!domain->db.Run(R"(INSERT INTO dom_instances (instance) VALUES (?1))", instance_key)) {
            // Master does not exist
            if (sqlite3_errcode(domain->db) == SQLITE_CONSTRAINT) {
                Span<const char> master = SplitStr(instance_key, '/');

                LogError("Master instance '%1' does not exist", master);
                *out_error = 404;
            }
            return false;
        }

        uint32_t permissions = (1u << RG_LEN(UserPermissionNames)) - 1;
        if (!domain->db.Run(R"(INSERT INTO dom_permissions (userid, instance, permissions)
                               VALUES (?1, ?2, ?3))",
                            default_userid, instance_key, permissions))
            return false;

        return true;
    });
    if (!success)
        return false;

    db_guard.Disable();
    return true;
}

int RunInit(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    const char *username = nullptr;
    const char *password = nullptr;
    const char *demo = nullptr;
    bool change_owner = false;
    uid_t owner_uid = 0;
    gid_t owner_gid = 0;
    const char *root_directory = nullptr;

    const auto print_usage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 init [options] [directory]%!0

Options:
    %!..+-u, --username <username>%!0    Name of default user
        %!..+--password <pwd>%!0         Password of default user

        %!..+--demo [<name>]%!0          Create default instance)", FelixTarget);

#ifndef _WIN32
        PrintLn(fp, R"(
    %!..+-o, --owner <owner>%!0          Change directory and file owner)");
#endif
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-u", "--username", OptionType::Value)) {
                username = opt.current_value;
            } else if (opt.Test("--password", OptionType::Value)) {
                password = opt.current_value;
            } else if (opt.Test("--demo", OptionType::OptionalValue)) {
                demo = opt.current_value ? opt.current_value : "demo";
#ifndef _WIN32
            } else if (opt.Test("-o", "--owner", OptionType::Value)) {
                change_owner = true;

                if (!FindPosixUser(opt.current_value, &owner_uid, &owner_gid))
                    return 1;
#endif
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        root_directory = opt.ConsumeNonOption();
        root_directory = NormalizePath(root_directory ? root_directory : ".", GetWorkingDirectory(), &temp_alloc).ptr;
    }

    // Errors and defaults
    if (password && !username) {
        LogError("Option --password cannot be used without --username");
        return 1;
    }

    // Drop created files and directories if anything fails
    HeapArray<const char *> directories;
    HeapArray<const char *> files;
    RG_DEFER_N(root_guard) {
        for (const char *filename: files) {
            UnlinkFile(filename);
        }
        for (Size i = directories.len - 1; i >= 0; i--) {
            UnlinkDirectory(directories[i]);
        }
    };

    // Make or check instance directory
    if (TestFile(root_directory)) {
        if (!IsDirectoryEmpty(root_directory)) {
            LogError("Directory '%1' is not empty", root_directory);
            return 1;
        }
    } else {
        if (!MakeDirectory(root_directory, false))
            return 1;
        directories.Append(root_directory);
    }
    if (change_owner && !ChangeFileOwner(root_directory, owner_uid, owner_gid))
        return 1;

    // Gather missing information
    if (!username) {
        username = Prompt("Admin: ", &temp_alloc);
        if (!username)
            return 1;
    }
    if (!CheckUserName(username))
        return 1;
    if (!password) {
        password = Prompt("Password: ", "*", &temp_alloc);
        if (!password)
            return 1;
    }
    if (!password[0]) {
        LogError("Password cannot be empty");
        return 1;
    }
    LogInfo();

    // Create backup key pair
    char backup_key[45];
    char decrypt_key[45];
    {
        RG_STATIC_ASSERT(crypto_box_PUBLICKEYBYTES == 32);
        RG_STATIC_ASSERT(crypto_box_SECRETKEYBYTES == 32);

        uint8_t pk[crypto_box_PUBLICKEYBYTES];
        uint8_t sk[crypto_box_SECRETKEYBYTES];
        crypto_box_keypair(pk, sk);

        sodium_bin2base64(backup_key, RG_SIZE(backup_key), pk, RG_SIZE(pk), sodium_base64_VARIANT_ORIGINAL);
        sodium_bin2base64(decrypt_key, RG_SIZE(decrypt_key), sk, RG_SIZE(sk), sodium_base64_VARIANT_ORIGINAL);
    }

    // Create domain
    DomainHolder domain;
    {
        const char *filename = Fmt(&temp_alloc, "%1%/goupile.ini", root_directory).ptr;
        files.Append(filename);

        StreamWriter writer(filename);
        Print(&writer, DefaultConfig, backup_key);
        if (!writer.Close())
            return 1;

        if (!LoadConfig(filename, &domain.config))
            return 1;
    }

    // Create directories
    {
        const auto make_directory = [&](const char *path) {
            if (!MakeDirectory(path))
                return false;
            directories.Append(path);
            if (change_owner && !ChangeFileOwner(path, owner_uid, owner_gid))
                return false;

            return true;
        };

        if (!make_directory(domain.config.instances_directory))
            return 1;
        if (!make_directory(domain.config.temp_directory))
            return 1;
        if (!make_directory(domain.config.backup_directory))
            return 1;
    }

    // Create database
    if (!domain.db.Open(domain.config.database_filename, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
        return 1;
    files.Append(domain.config.database_filename);
    if (!MigrateDomain(&domain.db, domain.config.instances_directory))
        return 1;
    if (change_owner && !ChangeFileOwner(domain.config.database_filename, owner_uid, owner_gid))
        return 1;

    // Create default admin user
    {
        char hash[crypto_pwhash_STRBYTES];
        if (!HashPassword(password, hash))
            return 1;

        // Create local key
        char local_key[45];
        {
            uint8_t buf[32];
            randombytes_buf(&buf, RG_SIZE(buf));
            sodium_bin2base64(local_key, RG_SIZE(local_key), buf, RG_SIZE(buf), sodium_base64_VARIANT_ORIGINAL);
        }

        if (!domain.db.Run(R"(INSERT INTO dom_users (userid, username, password_hash, admin, local_key)
                              VALUES (1, ?1, ?2, 1, ?3))", username, hash, local_key))
            return 1;
    }

    // Create default instance
    {
        int dummy;
        if (demo && !CreateInstance(&domain, demo, demo, 1, true, &dummy))
            return 1;
    }

    if (!domain.db.Close())
        return 1;

    LogInfo();
    LogInfo("Backup decryption key: %!..+%1%!0", decrypt_key);
    LogInfo();
    LogInfo("You need this key to restore Goupile backups, %!..+you must not lose it!%!0");
    LogInfo("There is no way to get it back, without it the backups are lost.");

    root_guard.Disable();
    return 0;
}

int RunMigrate(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    const char *config_filename = "goupile.ini";

    const auto print_usage = [&](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 migrate [options]%!0

Options:
    %!..+-C, --config_file <file>%!0     Set configuration file
                                 %!D..(default: %2)%!0)", FelixTarget, config_filename);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config_file", OptionType::Value)) {
                config_filename = opt.current_value;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }
    }

    DomainConfig config;
    if (!LoadConfig(config_filename, &config))
        return 1;

    // Migrate and open main database
    sq_Database db;
    if (!db.Open(config.database_filename, SQLITE_OPEN_READWRITE))
        return 1;
    if (!MigrateDomain(&db, config.instances_directory))
        return 1;

    // Migrate instances
    {
        sq_Statement stmt;
        if (!db.Prepare("SELECT instance FROM dom_instances", &stmt))
            return 1;

        bool success = true;

        while (stmt.Next()) {
            const char *key = (const char *)sqlite3_column_text(stmt, 0);
            const char *filename = config.GetInstanceFileName(key, &temp_alloc);

            success &= MigrateInstance(filename);
        }
        if (!stmt.IsValid())
            return 1;

        if (!success)
            return 1;
    }

    if (!db.Close())
        return 1;

    return 0;
}

int RunUnseal(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    const char *archive_filename = nullptr;
    const char *output_filename = nullptr;
    const char *decrypt_key = nullptr;

    const auto print_usage = [&](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 unseal <archive_file>%!0

Options:
    %!..+-O, --output_file <file>%!0      Set output file
    %!..+-k, --key <key>%!0               Set decryption key)", FelixTarget);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-O", "--output_file", OptionType::Value)) {
                output_filename = opt.current_value;
            } else if (opt.Test("-k", "--key", OptionType::Value)) {
                decrypt_key = opt.current_value;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        archive_filename = opt.ConsumeNonOption();
        if (!archive_filename) {
            LogError("No archive filename provided");
            return 1;
        }
    }

    if (!output_filename) {
        Span<const char> extension = GetPathExtension(archive_filename);
        Span<const char> name = MakeSpan(archive_filename, extension.ptr - archive_filename);

        output_filename = Fmt(&temp_alloc, "%1.zip", name).ptr;
    }
    if (TestFile(output_filename)) {
        LogError("File '%1' already exists", output_filename);
        return 1;
    }

    if (!decrypt_key) {
        decrypt_key = Prompt("Decryption key: ", "*", &temp_alloc);
        if (!decrypt_key)
            return 1;
    }

    StreamReader reader(archive_filename);
    StreamWriter writer(output_filename, (int)StreamWriterFlag::Atomic |
                                         (int)StreamWriterFlag::Exclusive);
    if (!reader.IsValid())
        return 1;
    if (!writer.IsValid())
        return 1;

    // Derive asymmetric keys
    uint8_t askey[crypto_box_SECRETKEYBYTES];
    uint8_t apkey[crypto_box_PUBLICKEYBYTES];
    {
        RG_STATIC_ASSERT(crypto_scalarmult_SCALARBYTES == crypto_box_SECRETKEYBYTES);
        RG_STATIC_ASSERT(crypto_scalarmult_BYTES == crypto_box_PUBLICKEYBYTES);

        size_t key_len;
        int ret = sodium_base642bin(askey, RG_SIZE(askey),
                                    decrypt_key, strlen(decrypt_key), nullptr, &key_len,
                                    nullptr, sodium_base64_VARIANT_ORIGINAL);
        if (ret || key_len != 32) {
            LogError("Malformed decryption key");
            return 1;
        }

        crypto_scalarmult_base(apkey, askey);
    }

    // Check signature and initialize symmetric decryption
    uint8_t skey[crypto_secretstream_xchacha20poly1305_KEYBYTES];
    crypto_secretstream_xchacha20poly1305_state state;
    {
        ArchiveIntro intro;
        if (reader.Read(RG_SIZE(intro), &intro) != RG_SIZE(intro)) {
            if (reader.IsValid()) {
                LogError("Truncated archive");
            }
            return 1;
        }

        if (strncmp(intro.signature, ARCHIVE_SIGNATURE, RG_SIZE(intro.signature)) != 0) {
            LogError("Unexpected archive signature");
            return 1;
        }
        if (intro.version != ARCHIVE_VERSION) {
            LogError("Unexpected archive version %1 (expected %2)", intro.version, ARCHIVE_VERSION);
            return 1;
        }

        if (crypto_box_seal_open(skey, intro.eskey, RG_SIZE(intro.eskey), apkey, askey) != 0) {
            LogError("Failed to unseal archive (wrong key?)");
            return 1;
        }
        if (crypto_secretstream_xchacha20poly1305_init_pull(&state, intro.header, skey) != 0) {
            LogError("Failed to initialize symmetric decryption (corrupt archive?)");
            return 1;
        }
    }

    for (;;) {
        LocalArray<uint8_t, 4096> cypher;
        cypher.len = reader.Read(cypher.data);
        if (cypher.len < 0)
            return 1;

        uint8_t buf[4096];
        unsigned long long buf_len = 5;
        uint8_t tag;
        if (crypto_secretstream_xchacha20poly1305_pull(&state, buf, &buf_len, &tag,
                                                       cypher.data, cypher.len, nullptr, 0) != 0) {
            LogError("Failed during symmetric decryption (corrupt archive?)");
            return 1;
        }

        if (!writer.Write(buf, (Size)buf_len))
            return 1;

        if (reader.IsEOF()) {
            if (tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
                LogError("Truncated archive");
                return 1;
            }
            break;
        }
    }
    if (!writer.Close())
        return 1;

    LogInfo("Decrypted archive: %!..+%1%!0", output_filename);
    return 0;
}

void HandleInstanceCreate(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to create instances");
            io->AttachError(403);
        }
        return;
    }

    if (gp_domain.CountInstances() >= MaxInstancesPerDomain) {
        LogError("This domain has too many instances");
        io->AttachError(403);
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        const char *instance_key;
        const char *title;
        bool demo;
        {
            bool valid = true;

            instance_key = values.FindValue("key", nullptr);
            if (!instance_key) {
                LogError("Missing 'key' parameter");
                valid = false;
            } else if (!CheckInstanceKey(instance_key)) {
                valid = false;
            }

            title = values.FindValue("title", instance_key);
            if (title && !title[0]) {
                LogError("Application title cannot be empty");
                valid = false;
            }

            valid &= ParseBool(values.FindValue("demo", "1"), &demo);

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        bool success = gp_domain.db.Transaction([&]() {
            // Log action
            int64_t time = GetUnixTime();
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5))",
                                  time, request.client_addr, "create_instance", session->username,
                                  instance_key))
                return false;

            int error;
            if (!CreateInstance(&gp_domain, instance_key, title, session->userid, demo, &error)) {
                io->AttachError(error);
                return false;
            }

            return true;
        });
        if (!success)
            return;

        if (!gp_domain.Sync())
            return;

        io->AttachText(200, "Done!");
    });
}

static bool BackupDatabase(sq_Database *src, const char *filename)
{
    sq_Database db;
    if (!db.Open(filename, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
        return false;

    sqlite3_backup *backup = sqlite3_backup_init(db, "main", *src, "main");
    if (!backup)
        return false;
    RG_DEFER { sqlite3_backup_finish(backup); };

restart:
    int ret = sqlite3_backup_step(backup, -1);

    if (ret != SQLITE_DONE) {
        if (ret == SQLITE_OK || ret == SQLITE_BUSY || ret == SQLITE_LOCKED) {
            WaitDelay(100);
            goto restart;
        } else {
            LogError("SQLite Error: %1", sqlite3_errstr(ret));
            return false;
        }
    }

    sqlite3_backup_finish(backup);
    backup = nullptr;

    return db.Close();
}

static bool BackupInstances(const InstanceHolder *filter, bool *out_conflict = nullptr)
{
    BlockAllocator temp_alloc;

    Span<InstanceHolder *> instances = gp_domain.LockInstances();
    RG_DEFER { gp_domain.UnlockInstances(); };

    if (out_conflict) {
        *out_conflict = false;
    }

    struct BackupEntry {
        sq_Database *db;
        const char *basename;
        const char *filename;
    };

    HeapArray<BackupEntry> entries;
    RG_DEFER {
        for (const BackupEntry &entry: entries) {
            UnlinkFile(entry.filename);
        }
    };

    // Make archive filename
    const char *archive_filename;
    {
        time_t mtime = (time_t)(GetUnixTime() / 1000);

#ifdef _WIN32
        struct tm mtime_tm;
        {
            errno_t err = _gmtime64_s(&mtime_tm, &mtime);
            if (err) {
                LogError("Failed to format current time: %1", strerror(err));
                return false;
            }
        }
#else
        struct tm mtime_tm;
        if (!gmtime_r(&mtime, &mtime_tm)) {
            LogError("Failed to format current time: %1", strerror(errno));
            return false;
        }
#endif

        char mtime_str[128];
        if (!strftime(mtime_str, RG_SIZE(mtime_str), "%Y%m%dT%H%M%S%z", &mtime_tm)) {
            LogError("Failed to format current time: strftime failed");
            return false;
        }

        HeapArray<char> buf(&temp_alloc);
        Fmt(&buf, "%1%/%2", gp_domain.config.backup_directory, mtime_str);
        if (filter) {
            Span<const char> basename = SplitStrReverseAny(filter->filename, RG_PATH_SEPARATORS);
            SplitStrReverse(basename, '.', &basename);

            Fmt(&buf, "_%1", basename);
        }
        Fmt(&buf, ".goupilebackup");

        archive_filename = buf.Leak().ptr;
    }

    // Open archive
    StreamWriter writer;
    if (!writer.Open(archive_filename, (int)StreamWriterFlag::Exclusive |
                                       (int)StreamWriterFlag::Atomic)) {
        if (out_conflict && errno == EEXIST) {
            *out_conflict = true;
        }
        return false;
    }

    // Generate backup entries
    entries.Append({&gp_domain.db, "goupile.db"});
    for (InstanceHolder *instance: instances) {
        if (filter == nullptr || instance == filter || instance->master == filter) {
            const char *basename = SplitStrReverseAny(instance->filename, RG_PATH_SEPARATORS).ptr;
            basename = Fmt(&temp_alloc, "instances%/%1", basename).ptr;

            entries.Append({&instance->db, basename});
        }
    }
    for (BackupEntry &entry: entries) {
        entry.filename = CreateTemporaryFile(gp_domain.config.temp_directory, "", nullptr, &temp_alloc);
    }

    // Backup databases
    Async async;
    for (const BackupEntry &entry: entries) {
        async.Run([&, entry]() { return BackupDatabase(entry.db, entry.filename); });
    }
    if (!async.Sync())
        return false;

    // Closure context for miniz write callback
    struct BackupContext {
        StreamWriter *writer;
        crypto_secretstream_xchacha20poly1305_state state;
        LocalArray<uint8_t, 4096 - crypto_secretstream_xchacha20poly1305_ABYTES> buf;
    };
    BackupContext ctx = {};
    ctx.writer = &writer;

    // Write archive intro
    {
        ArchiveIntro intro = {};

        CopyString(ARCHIVE_SIGNATURE, intro.signature);
        intro.version = ARCHIVE_VERSION;

        uint8_t skey[crypto_secretstream_xchacha20poly1305_KEYBYTES];
        crypto_secretstream_xchacha20poly1305_keygen(skey);
        if (crypto_secretstream_xchacha20poly1305_init_push(&ctx.state, intro.header, skey) != 0) {
            LogError("Failed to initialize symmetric encryption");
            return false;
        }
        if (crypto_box_seal(intro.eskey, skey, RG_SIZE(skey), gp_domain.config.backup_key) != 0) {
            LogError("Failed to seal symmetric key");
            return false;
        }

        if (!writer.Write(&intro, RG_SIZE(intro)))
            return false;
    }

    // Init ZIP compressor
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pWrite = [](void *udata, mz_uint64, const void *buf, size_t len) {
        BackupContext *ctx = (BackupContext *)udata;
        size_t copy = len;

        while (len) {
            size_t copy_len = std::min(len, (size_t)ctx->buf.Available());
            memcpy(ctx->buf.end(), buf, copy_len);
            ctx->buf.len += (Size)copy_len;

            if (!ctx->buf.Available()) {
                uint8_t cypher[4096];
                unsigned long long cypher_len;
                if (crypto_secretstream_xchacha20poly1305_push(&ctx->state, cypher, &cypher_len,
                                                               ctx->buf.data, ctx->buf.len, nullptr, 0, 0) != 0) {
                    LogError("Failed during symmetric encryption");
                    return (size_t)-1;
                }
                if (!ctx->writer->Write(cypher, (Size)cypher_len))
                    return (size_t)-1;
                ctx->buf.len = 0;
            }

            buf = (const void *)((const uint8_t *)buf + copy_len);
            len -= copy_len;
        }

        return copy;
    };
    zip.m_pIO_opaque = &ctx;
    if (!mz_zip_writer_init(&zip, 0)) {
        LogError("Failed to create ZIP archive: %1", mz_zip_get_error_string(zip.m_last_error));
        return false;
    }
    RG_DEFER { mz_zip_writer_end(&zip); };

    // Add databases to ZIP archive
    for (const BackupEntry &entry: entries) {
        if (!mz_zip_writer_add_file(&zip, entry.basename, entry.filename, nullptr, 0, MZ_BEST_SPEED)) {
            if (zip.m_last_error != MZ_ZIP_WRITE_CALLBACK_FAILED) {
                LogError("Failed to compress '%1': %2", entry.basename, mz_zip_get_error_string(zip.m_last_error));
            }
            return false;
        }
    }

    // Finalize ZIP
    if (!mz_zip_writer_finalize_archive(&zip)) {
        if (zip.m_last_error != MZ_ZIP_WRITE_CALLBACK_FAILED) {
            LogError("Failed to finalize ZIP archive: %1", mz_zip_get_error_string(zip.m_last_error));
        }
        return false;
    }

    // Finalize encryption
    {
        uint8_t cypher[4096];
        unsigned long long cypher_len;
        if (crypto_secretstream_xchacha20poly1305_push(&ctx.state, cypher, &cypher_len, ctx.buf.data, ctx.buf.len, nullptr, 0,
                                                       crypto_secretstream_xchacha20poly1305_TAG_FINAL) != 0) {
            LogError("Failed during symmetric encryption");
            return false;
        }

        if (!writer.Write(cypher, (Size)cypher_len))
            return false;
     }

    // Flush buffers and rename atomically
    if (!writer.Close())
        return false;

    return true;
}

void HandleInstanceDelete(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to delete instances");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        const char *instance_key = values.FindValue("key", nullptr);
        if (!instance_key) {
            LogError("Missing 'key' parameter");
            io->AttachError(422);
            return;
        }

        InstanceHolder *instance = gp_domain.Ref(instance_key);
        if (!instance) {
            LogError("Instance '%1' does not exist", instance_key);
            io->AttachError(404);
            return;
        }
        RG_DEFER_N(ref_guard) { instance->Unref(); };

        bool conflict;
        if (!BackupInstances(instance, &conflict)) {
            if (conflict) {
                io->AttachError(409, "Archive already exists");
            }
            return;
        }

        bool success = gp_domain.db.Transaction([&]() {
            int64_t time = GetUnixTime();

            for (Size i = instance->slaves.len - 1; i >= 0; i--) {
                InstanceHolder *slave = instance->slaves[i];

                if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                         VALUES (?1, ?2, ?3, ?4, ?5))",
                                      time, request.client_addr, "delete_instance", session->username,
                                      slave->key))
                    return false;
                if (!gp_domain.db.Run("DELETE FROM dom_instances WHERE instance = ?1", slave->key))
                    return false;
            }

            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5))",
                                  time, request.client_addr, "delete_instance", session->username,
                                  instance_key))
                return false;
            if (!gp_domain.db.Run("DELETE FROM dom_instances WHERE instance = ?1", instance_key))
                return false;

            return true;
        });
        if (!success)
            return;

        // Copy filenames to avoid use-after-free
        HeapArray<const char *> unlink_filenames;
        {
            for (const InstanceHolder *slave: instance->slaves) {
                const char *filename = DuplicateString(slave->filename, &io->allocator).ptr;
                unlink_filenames.Append(filename);
            }

            const char *filename = DuplicateString(instance->filename, &io->allocator).ptr;
            unlink_filenames.Append(filename);
        }

        instance->Unref();
        ref_guard.Disable();
        if (!gp_domain.Sync())
            return;

        bool complete = true;
        for (const char *filename: unlink_filenames) {
            // Not much we can do if this fails to succeed anyway; the backup is okay and the
            // instance is deleted. We're mostly successful and we can't go back.
            complete &= UnlinkFile(filename);
        }

        if (complete) {
            io->AttachText(200, "Done!");
        } else {
            io->AttachText(202, "Done, but with leftover databases");
        }
    });
}

void HandleInstanceConfigure(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to configure instances");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        const char *instance_key = values.FindValue("key", nullptr);
        if (!instance_key) {
            LogError("Missing 'key' parameter");
            io->AttachError(422);
            return;
        }

        InstanceHolder *instance = gp_domain.Ref(instance_key);
        if (!instance) {
            LogError("Instance '%1' does not exist", instance_key);
            io->AttachError(404);
            return;
        }
        RG_DEFER_N(ref_guard) { instance->Unref(); };

        // Parse new configuration values
        decltype(InstanceHolder::config) config = instance->config;
        {
            bool valid = true;
            char buf[128];

            if (const char *str = values.FindValue("title", nullptr); str) {
                config.title = str;

                if (!str[0]) {
                    LogError("Application name cannot be empty");
                    valid = false;
                }
            }

            if (const char *str = values.FindValue("use_offline", nullptr); str) {
                Span<const char> str2 = ConvertFromJsonName(str, buf);
                valid &= ParseBool(str2, &config.use_offline);
            }

            if (const char *str = values.FindValue("sync_mode", nullptr); str) {
                Span<const char> str2 = ConvertFromJsonName(str, buf);
                if (!OptionToEnum(SyncModeNames, str2, &config.sync_mode)) {
                    LogError("Unknown sync mode '%1'", str);
                    valid = false;
                }
            }

            config.backup_key = values.FindValue("backup_key", config.backup_key);
            if (config.backup_key && !config.backup_key[0])
                config.backup_key = nullptr;

            if (const char *str = values.FindValue("auto_userid", nullptr); str) {
                if (str[0]) {
                    valid &= ParseInt(str, &config.auto_userid);

                    if (config.auto_userid <= 0) {
                        LogError("Invalid automatic user ID");
                        valid = false;
                    }
                } else {
                    config.auto_userid = 0;
                }
            }

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        // Write new configuration to database
        bool success = instance->db.Transaction([&]() {
            // Log action
            int64_t time = GetUnixTime();
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5))",
                                  time, request.client_addr, "edit_instance", session->username,
                                  instance_key))
                return false;

            const char *sql = "UPDATE fs_settings SET value = ?2 WHERE key = ?1";
            bool success = true;

            success &= instance->db.Run(sql, "Title", config.title);
            if (instance->master == instance) {
                success &= instance->db.Run(sql, "UseOffline", 0 + config.use_offline);
                success &= instance->db.Run(sql, "SyncMode", SyncModeNames[(int)config.sync_mode]);
                success &= instance->db.Run(sql, "BackupKey", config.backup_key);
            }
            success &= instance->db.Run(sql, "AutoUserID", config.auto_userid ? sq_Binding(config.auto_userid) : sq_Binding());
            if (!success)
                return false;

            if (!gp_domain.db.Run(R"(UPDATE dom_instances SET generation = generation + 1
                                     WHERE instance = ?1)", instance->key))
                return false;

            return true;
        });
        if (!success)
            return;

        // Avoid deadlock
        instance->Unref();
        ref_guard.Disable();

        if (!gp_domain.Sync())
            return;

        io->AttachText(200, "Done!");
    });
}

void HandleInstanceList(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to list instances");
            io->AttachError(403);
        }
        return;
    }

    Span<InstanceHolder *> instances = gp_domain.LockInstances();
    RG_DEFER { gp_domain.UnlockInstances(); };

    // Export data
    http_JsonPageBuilder json;
    if (!json.Init(io))
        return;
    char buf[128];

    json.StartArray();
    for (InstanceHolder *instance: instances) {
        json.StartObject();

        json.Key("key"); json.String(instance->key.ptr);
        if (instance->master != instance) {
            json.Key("master"); json.String(instance->master->key.ptr);
        } else {
            json.Key("slaves"); json.Int64(instance->slaves.len);
        }
        json.Key("config"); json.StartObject();
            json.Key("title"); json.String(instance->config.title);
            json.Key("use_offline"); json.Bool(instance->config.use_offline);
            {
                Span<const char> str = ConvertToJsonName(SyncModeNames[(int)instance->config.sync_mode], buf);
                json.Key("sync_mode"); json.String(str.ptr, (size_t)str.len);
            }
            if (instance->config.backup_key) {
                json.Key("backup_key"); json.String(instance->config.backup_key);
            }
            if (instance->config.auto_userid > 0) {
                json.Key("auto_userid"); json.Int64(instance->config.auto_userid);
            }
        json.EndObject();

        json.EndObject();
    }
    json.EndArray();

    json.Finish();
}

static bool ParsePermissionList(Span<const char> remain, uint32_t *out_permissions)
{
    uint32_t permissions = 0;

    while (remain.len) {
        Span<const char> js_perm = TrimStr(SplitStr(remain, ',', &remain), " ");

        if (js_perm.len) {
            char buf[128];

            js_perm = ConvertFromJsonName(js_perm, buf);

            UserPermission perm;
            if (!OptionToEnum(UserPermissionNames, ConvertFromJsonName(js_perm, buf), &perm)) {
                LogError("Unknown permission '%1'", js_perm);
                return false;
            }

            permissions |= 1 << (int)perm;
        }
    }

    *out_permissions = permissions;
    return true;
}

void HandleInstanceAssign(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to delete users");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        int64_t userid;
        const char *instance;
        uint32_t permissions;
        {
            bool valid = true;

            if (const char *str = values.FindValue("userid", nullptr); str) {
                valid &= ParseInt(str, &userid);
            } else {
                LogError("Missing 'userid' parameter");
                valid = false;
            }

            instance = values.FindValue("instance", nullptr);
            if (!instance) {
                LogError("Missing 'instance' parameter");
                valid = false;
            }

            if (const char *str = values.FindValue("permissions", nullptr); str) {
                valid &= ParsePermissionList(str, &permissions);
            } else {
                LogError("Missing 'permissions' parameter");
                valid = false;
            }

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        gp_domain.db.Transaction([&]() {
            // Does instance exist?
            {
                sq_Statement stmt;
                if (!gp_domain.db.Prepare("SELECT instance FROM dom_instances WHERE instance = ?1", &stmt))
                    return false;
                sqlite3_bind_text(stmt, 1, instance, -1, SQLITE_STATIC);

                if (!stmt.Next()) {
                    if (stmt.IsValid()) {
                        LogError("Instance '%1' does not exist", instance);
                        io->AttachError(404);
                    }
                    return false;
                }
            }

            // Does user exist?
            const char *username;
            {
                sq_Statement stmt;
                if (!gp_domain.db.Prepare("SELECT username FROM dom_users WHERE userid = ?1", &stmt))
                    return false;
                sqlite3_bind_int64(stmt, 1, userid);

                if (!stmt.Next()) {
                    if (stmt.IsValid()) {
                        LogError("User ID '%1' does not exist", userid);
                        io->AttachError(404);
                    }
                    return false;
                }

                username = DuplicateString((const char *)sqlite3_column_text(stmt, 0), &io->allocator).ptr;
            }

            // Log action
            int64_t time = GetUnixTime();
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5 || '+' || ?6 || ':' || ?7))",
                                  time, request.client_addr, "assign_user", session->username,
                                  instance, username, permissions))
                return false;

            // Adjust permissions
            if (permissions) {
                if (!gp_domain.db.Run(R"(INSERT INTO dom_permissions (instance, userid, permissions)
                                         VALUES (?1, ?2, ?3)
                                         ON CONFLICT(instance, userid)
                                             DO UPDATE SET permissions = excluded.permissions)",
                                      instance, userid, permissions))
                    return false;
            } else {
                if (!gp_domain.db.Run("DELETE FROM dom_permissions WHERE instance = ?1 AND userid = ?2",
                                      instance, userid))
                    return false;
            }

            InvalidateUserTokens(userid);

            io->AttachText(200, "Done!");
            return true;
        });
    });
}

void HandleInstancePermissions(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to list users");
            io->AttachError(403);
        }
        return;
    }

    const char *instance_key = request.GetQueryValue("key");
    if (!instance_key) {
        LogError("Missing 'key' parameter");
        io->AttachError(422);
        return;
    }

    sq_Statement stmt;
    if (!gp_domain.db.Prepare(R"(SELECT userid, permissions FROM dom_permissions
                                 WHERE instance = ?1
                                 ORDER BY instance)", &stmt))
        return;
    sqlite3_bind_text(stmt, 1, instance_key, -1, SQLITE_STATIC);

    // Export data
    http_JsonPageBuilder json;
    if (!json.Init(io))
        return;

    json.StartObject();
    while (stmt.Next()) {
        int64_t userid = sqlite3_column_int64(stmt, 0);
        uint32_t permissions = (uint32_t)sqlite3_column_int64(stmt, 1);
        char buf[128];

        json.Key(Fmt(buf, "%1", userid).ptr); json.StartArray();
        for (Size i = 0; i < RG_LEN(UserPermissionNames); i++) {
            if (permissions & (1 << i)) {
                Span<const char> str = ConvertToJsonName(UserPermissionNames[i], buf);
                json.String(str.ptr, (size_t)str.len);
            }
        }
        json.EndArray();
    }
    if (!stmt.IsValid())
        return;
    json.EndObject();

    json.Finish();
}

void HandleArchiveCreate(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to create archives");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        bool conflict;
        if (!BackupInstances(nullptr, &conflict)) {
            if (conflict) {
                io->AttachError(409, "Archive already exists");
            }
            return;
        }

        io->AttachText(200, "Done!");
    });
}

void HandleArchiveDelete(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to delete archives");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        const char *basename = values.FindValue("filename", nullptr);
        if (!basename) {
            LogError("Missing 'filename' paramreter");
            io->AttachError(422);
            return;
        }

        // Safety checks
        if (PathIsAbsolute(basename)) {
            LogError("Path must not be absolute");
            io->AttachError(403);
            return;
        }
        if (PathContainsDotDot(basename)) {
            LogError("Path must not contain any '..' component");
            io->AttachError(403);
            return;
        }

        const char *filename = Fmt(&io->allocator, "%1%/%2", gp_domain.config.backup_directory, basename).ptr;

        if (!TestFile(filename, FileType::File)) {
            io->AttachError(404);
            return;
        }
        if (!UnlinkFile(filename))
            return;

        io->AttachText(200, "Done!");
    });
}

void HandleArchiveList(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to list archives");
            io->AttachError(403);
        }
        return;
    }

    // Export data
    http_JsonPageBuilder json;
    if (!json.Init(io))
        return;
    HeapArray<char> buf;

    json.StartArray();
    EnumStatus status = EnumerateDirectory(gp_domain.config.backup_directory, "*.goupilebackup", -1,
                                           [&](const char *basename, FileType) {
        buf.RemoveFrom(0);

        const char *filename = Fmt(&buf, "%1%/%2", gp_domain.config.backup_directory, basename).ptr;

        FileInfo file_info;
        if (!StatFile(filename, &file_info))
            return false;

        // Don't list backups currently in creation
        if (file_info.size) {
            json.StartObject();
            json.Key("filename"); json.String(basename);
            json.Key("size"); json.Int64(file_info.size);
            json.EndObject();
        }

        return true;
    });
    if (status != EnumStatus::Done)
        return;
    json.EndArray();

    json.Finish();
}

void HandleArchiveDownload(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to download archives");
            io->AttachError(403);
        }
        return;
    }

    const char *basename = request.GetQueryValue("filename");
    if (!basename) {
        LogError("Missing 'filename' paramreter");
        io->AttachError(422);
        return;
    }

    // Safety checks
    if (PathIsAbsolute(basename)) {
        LogError("Path must not be absolute");
        io->AttachError(403);
        return;
    }
    if (PathContainsDotDot(basename)) {
        LogError("Path must not contain any '..' component");
        io->AttachError(403);
        return;
    }
    if (GetPathExtension(basename) != ".goupilebackup") {
        LogError("Path must end with '.goupilebackup' extension");
        io->AttachError(403);
        return;
    }

    const char *filename = Fmt(&io->allocator, "%1%/%2", gp_domain.config.backup_directory, basename).ptr;

    FileInfo file_info;
    if (!StatFile(filename, &file_info)) {
        LogError("Cannot find archive '%1'", basename);
        io->AttachError(404);
        return;
    }
    if (file_info.type != FileType::File) {
        LogError("Path does not point to a file");
        io->AttachError(403);
        return;
    }

    int fd = OpenDescriptor(filename, (int)OpenFileFlag::Read);
    if (fd < 0)
        return;
#ifdef _WIN32
    RG_DEFER_N(fd_guard) { _close(fd); };
#else
    RG_DEFER_N(fd_guard) { close(fd); };
#endif

    MHD_Response *response = MHD_create_response_from_fd((uint64_t)file_info.size, fd);
    if (!response)
        return;
    fd_guard.Disable();
    io->AttachResponse(200, response);

    // Ask browser to download
    {
        const char *disposition = Fmt(&io->allocator, "attachment; filename=\"%1\"", basename).ptr;
        io->AddHeader("Content-Disposition", disposition);
    }
}

void HandleArchiveUpload(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to upload archives");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        int64_t time = GetUnixTime();
        const char *filename = Fmt(&io->allocator, "%1%/upload_%2@%3.goupilebackup",
                                   gp_domain.config.backup_directory, session->username, time).ptr;

        StreamWriter writer;
        if (!writer.Open(filename, (int)StreamWriterFlag::Exclusive |
                                   (int)StreamWriterFlag::Atomic)) {
            if (errno == EEXIST) {
                LogError("An archive already exists with this name");
                io->AttachError(409);
            }
            return;
        }

        StreamReader reader;
        if (!io->OpenForRead(Megabytes(128), &reader))
            return;

        // Read and store
        while (!reader.IsEOF()) {
            LocalArray<uint8_t, 16384> buf;
            buf.len = reader.Read(buf.data);
            if (buf.len < 0)
                return;

            if (!writer.Write(buf))
                return;
        }

        if (!writer.Close())
            return;

        io->AttachText(200, "Done!");
    });
}

void HandleArchiveRestore(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to upload archives");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        const char *basename = values.FindValue("filename", nullptr);
        if (!basename) {
            LogError("Missing 'filename' paramreter");
            io->AttachError(422);
            return;
        }

        const char *decrypt_key = values.FindValue("key", nullptr);
        if (!decrypt_key) {
            LogError("Missing 'key' parameter");
            io->AttachError(422);
            return;
        }

        // Safety checks
        if (PathIsAbsolute(basename)) {
            LogError("Path must not be absolute");
            io->AttachError(403);
            return;
        }
        if (PathContainsDotDot(basename)) {
            LogError("Path must not contain any '..' component");
            io->AttachError(403);
            return;
        }
        if (GetPathExtension(basename) != ".goupilebackup") {
            LogError("Path must end with '.goupilebackup' extension");
            io->AttachError(403);
            return;
        }

        const char *filename = Fmt(&io->allocator, "%1%/%2", gp_domain.config.backup_directory, basename).ptr;

        // Create temporary file
        FILE *fp = nullptr;
        const char *tmp_filename = CreateTemporaryFile(gp_domain.config.temp_directory, "", ".tmp",
                                                       &io->allocator, &fp);
        if (!tmp_filename)
            return;
        RG_DEFER {
            fclose(fp);
            UnlinkFile(tmp_filename);
        };

        StreamReader reader(filename);
        StreamWriter writer(fp, tmp_filename);
        if (!reader.IsValid()) {
            if (errno == ENOENT) {
                LogError("Archive '%1' does not exist", basename);
                io->AttachError(404);
            }
            return;
        }
        if (!writer.IsValid())
            return;

        // Derive asymmetric keys
        uint8_t askey[crypto_box_SECRETKEYBYTES];
        uint8_t apkey[crypto_box_PUBLICKEYBYTES];
        {
            RG_STATIC_ASSERT(crypto_scalarmult_SCALARBYTES == crypto_box_SECRETKEYBYTES);
            RG_STATIC_ASSERT(crypto_scalarmult_BYTES == crypto_box_PUBLICKEYBYTES);

            size_t key_len;
            int ret = sodium_base642bin(askey, RG_SIZE(askey),
                                        decrypt_key, strlen(decrypt_key), nullptr, &key_len,
                                        nullptr, sodium_base64_VARIANT_ORIGINAL);
            if (ret || key_len != 32) {
                LogError("Malformed decryption key");
                io->AttachError(422);
                return;
            }

            crypto_scalarmult_base(apkey, askey);
        }

        // Check signature and initialize symmetric decryption
        uint8_t skey[crypto_secretstream_xchacha20poly1305_KEYBYTES];
        crypto_secretstream_xchacha20poly1305_state state;
        {
            ArchiveIntro intro;
            if (reader.Read(RG_SIZE(intro), &intro) != RG_SIZE(intro)) {
                if (reader.IsValid()) {
                    LogError("Truncated archive");
                    io->AttachError(422);
                }
                return;
            }

            if (strncmp(intro.signature, ARCHIVE_SIGNATURE, RG_SIZE(intro.signature)) != 0) {
                LogError("Unexpected archive signature");
                io->AttachError(422);
                return;
            }
            if (intro.version != ARCHIVE_VERSION) {
                LogError("Unexpected archive version %1 (expected %2)", intro.version, ARCHIVE_VERSION);
                io->AttachError(422);
                return;
            }

            if (crypto_box_seal_open(skey, intro.eskey, RG_SIZE(intro.eskey), apkey, askey) != 0) {
                LogError("Failed to unseal archive (wrong key?)");
                io->AttachError(403);
                return;
            }
            if (crypto_secretstream_xchacha20poly1305_init_pull(&state, intro.header, skey) != 0) {
                LogError("Failed to initialize symmetric decryption (corrupt archive?)");
                io->AttachError(422);
                return;
            }
        }

        for (;;) {
            LocalArray<uint8_t, 4096> cypher;
            cypher.len = reader.Read(cypher.data);
            if (cypher.len < 0)
                return;

            uint8_t buf[4096];
            unsigned long long buf_len = 5;
            uint8_t tag;
            if (crypto_secretstream_xchacha20poly1305_pull(&state, buf, &buf_len, &tag,
                                                           cypher.data, cypher.len, nullptr, 0) != 0) {
                LogError("Failed during symmetric decryption (corrupt archive?)");
                io->AttachError(422);
                return;
            }

            if (!writer.Write(buf, (Size)buf_len))
                return;

            if (reader.IsEOF()) {
                if (tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
                    LogError("Truncated archive");
                    io->AttachError(422);
                    return;
                }
                break;
            }
        }
        if (!writer.Close())
            return;

        // XXX: Finish archive restoration
        io->AttachError(501);
    });
}

void HandleUserCreate(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to create users");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        const char *username;
        const char *password;
        const char *email;
        const char *phone;
        bool admin;
        {
            bool valid = true;

            username = values.FindValue("username", nullptr);
            password = values.FindValue("password", nullptr);
            email = values.FindValue("email", nullptr);
            phone = values.FindValue("phone", nullptr);
            if (!username || !password) {
                LogError("Missing 'username' or 'password' parameter");
                valid = false;
            }
            if (username && !CheckUserName(username)) {
                valid = false;
            }
            if (password && !password[0]) {
                LogError("Empty password is not allowed");
                valid = false;
            }
            if (email && !strchr(email, '@')) {
                LogError("Invalid email address format");
                valid = false;
            }
            if (phone && phone[0] != '+') {
                LogError("Invalid phone number format (prefix is mandatory)");
                valid = false;
            }

            valid &= ParseBool(values.FindValue("admin", "0"), &admin);

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        // Hash password
        char hash[crypto_pwhash_STRBYTES];
        if (!HashPassword(password, hash))
            return;

        // Create local key
        char local_key[45];
        {
            uint8_t buf[32];
            randombytes_buf(&buf, RG_SIZE(buf));
            sodium_bin2base64(local_key, RG_SIZE(local_key), buf, RG_SIZE(buf), sodium_base64_VARIANT_ORIGINAL);
        }

        gp_domain.db.Transaction([&]() {
            // Check for existing user
            {
                sq_Statement stmt;
                if (!gp_domain.db.Prepare("SELECT admin FROM dom_users WHERE username = ?1", &stmt))
                    return false;
                sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

                if (stmt.Next()) {
                    LogError("User '%1' already exists", username);
                    io->AttachError(409);
                    return false;
                } else if (!stmt.IsValid()) {
                    return false;
                }
            }

            // Log action
            int64_t time = GetUnixTime();
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5))",
                                  time, request.client_addr, "create_user", session->username,
                                  username))
                return false;

            // Create user
            if (!gp_domain.db.Run(R"(INSERT INTO dom_users (username, password_hash, email, phone, admin, local_key)
                                     VALUES (?1, ?2, ?3, ?4, ?5, ?6))",
                                  username, hash, email, phone, 0 + admin, local_key))
                return false;

            io->AttachText(200, "Done!");
            return true;
        });
    });
}

void HandleUserEdit(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to edit users");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        int64_t userid;
        const char *username;
        const char *password;
        const char *email;
        const char *phone;
        bool admin, set_admin = false;
        {
            bool valid = true;

            // User ID
            if (const char *str = values.FindValue("userid", nullptr); str) {
                valid &= ParseInt(str, &userid);
            } else {
                LogError("Missing 'userid' parameter");
                valid = false;
            }

            username = values.FindValue("username", nullptr);
            password = values.FindValue("password", nullptr);
            email = values.FindValue("email", nullptr);
            phone = values.FindValue("phone", nullptr);
            if (username && !CheckUserName(username)) {
                valid = false;
            }
            if (password && !password[0]) {
                LogError("Empty password is not allowed");
                valid = false;
            }
            if (email && !strchr(email, '@')) {
                LogError("Invalid email address format");
                valid = false;
            }
            if (phone && phone[0] != '+') {
                LogError("Invalid phone number format (prefix is mandatory)");
                valid = false;
            }

            if (const char *str = values.FindValue("admin", nullptr); str) {
                valid &= ParseBool(str, &admin);
                set_admin = true;
            }

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        // Safety check
        if (userid == session->userid && set_admin && admin != !!session->admin_until) {
            LogError("You cannot change your admin privileges");
            io->AttachError(403);
            return;
        }

        // Hash password
        char hash[crypto_pwhash_STRBYTES];
        if (password && !HashPassword(password, hash))
            return;

        gp_domain.db.Transaction([&]() {
            // Check for existing user
            {
                sq_Statement stmt;
                if (!gp_domain.db.Prepare("SELECT rowid FROM dom_users WHERE userid = ?1", &stmt))
                    return false;
                sqlite3_bind_int64(stmt, 1, userid);

                if (!stmt.Next()) {
                    if (stmt.IsValid()) {
                        LogError("User ID '%1' does not exist", userid);
                        io->AttachError(404);
                    }
                    return false;
                }
            }

            // Log action
            int64_t time = GetUnixTime();
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5))",
                                  time, request.client_addr, "edit_user", session->username,
                                  username))
                return false;

            // Edit user
            if (username && !gp_domain.db.Run("UPDATE dom_users SET username = ?2 WHERE userid = ?1", userid, username))
                return false;
            if (password && !gp_domain.db.Run("UPDATE dom_users SET password_hash = ?2 WHERE userid = ?1", userid, hash))
                return false;
            if (email && !gp_domain.db.Run("UPDATE dom_users SET email = ?2 WHERE userid = ?1", userid, email))
                return false;
            if (phone && !gp_domain.db.Run("UPDATE dom_users SET phone = ?2 WHERE userid = ?1", userid, phone))
                return false;
            if (set_admin && !gp_domain.db.Run("UPDATE dom_users SET admin = ?2 WHERE userid = ?1", userid, 0 + admin))
                return false;

            io->AttachText(200, "Done!");
            return true;
        });
    });
}

void HandleUserDelete(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to delete users");
            io->AttachError(403);
        }
        return;
    }

    io->RunAsync([=]() {
        HashMap<const char *, const char *> values;
        if (!io->ReadPostValues(&io->allocator, &values)) {
            io->AttachError(422);
            return;
        }

        // Read POST values
        int64_t userid;
        {
            bool valid = true;

            // User ID
            if (const char *str = values.FindValue("userid", nullptr); str) {
                valid &= ParseInt(str, &userid);
            } else {
                LogError("Missing 'userid' parameter");
                valid = false;
            }

            if (!valid) {
                io->AttachError(422);
                return;
            }
        }

        // Safety check
        if (userid == session->userid) {
            LogError("You cannot delete yourself");
            io->AttachError(403);
            return;
        }

        gp_domain.db.Transaction([&]() {
            sq_Statement stmt;
            if (!gp_domain.db.Prepare("SELECT username, local_key FROM dom_users WHERE userid = ?1", &stmt))
                return false;
            sqlite3_bind_int64(stmt, 1, userid);

            if (!stmt.Next()) {
                if (stmt.IsValid()) {
                    LogError("User ID '%1' does not exist", userid);
                    io->AttachError(404);
                }
                return false;
            }

            const char *username = (const char *)sqlite3_column_text(stmt, 0);
            const char *local_key = (const char *)sqlite3_column_text(stmt, 1);
            int64_t time = GetUnixTime();

            // Log action
            if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username, details)
                                     VALUES (?1, ?2, ?3, ?4, ?5 || ':' || ?6))",
                                  time, request.client_addr, "delete_user", session->username,
                                  username, local_key))
                return false;

            if (!gp_domain.db.Run("DELETE FROM dom_users WHERE userid = ?1", userid))
                return false;

            io->AttachError(200, "Done!");
            return true;
        });
    });
}

void HandleUserList(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(nullptr, request, io);

    if (!session) {
        LogError("User is not logged in");
        io->AttachError(401);
        return;
    }
    if (!session->IsAdmin()) {
        if (session->admin_until) {
            LogError("Admin user needs to confirm identity");
            io->AttachError(401);
        } else {
            LogError("Non-admin users are not allowed to list users");
            io->AttachError(403);
        }
        return;
    }

    sq_Statement stmt;
    if (!gp_domain.db.Prepare(R"(SELECT userid, username, email, phone, admin FROM dom_users
                                 ORDER BY username)", &stmt))
        return;

    // Export data
    http_JsonPageBuilder json;
    if (!json.Init(io))
        return;

    json.StartArray();
    while (stmt.Next()) {
        json.StartObject();
        json.Key("userid"); json.Int64(sqlite3_column_int64(stmt, 0));
        json.Key("username"); json.String((const char *)sqlite3_column_text(stmt, 1));
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            json.Key("email"); json.String((const char *)sqlite3_column_text(stmt, 2));
        } else {
            json.Key("email"); json.Null();
        }
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            json.Key("phone"); json.String((const char *)sqlite3_column_text(stmt, 3));
        } else {
            json.Key("phone"); json.Null();
        }
        json.Key("admin"); json.Bool(sqlite3_column_int(stmt, 4));
        json.EndObject();
    }
    if (!stmt.IsValid())
        return;
    json.EndArray();

    json.Finish();
}

}
