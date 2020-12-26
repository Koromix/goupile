// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "../../core/libcc/libcc.hh"
#include "instance.hh"
#include "../../core/libwrap/sqlite.hh"
#include "../../web/libhttp/libhttp.hh"

namespace RG {

extern const int DomainVersion;

struct DomainConfig {
    const char *database_filename = nullptr;
    const char *instances_directory = nullptr;
    const char *temp_directory = nullptr;

    const char *demo_user = nullptr;

    // XXX: Restore http_Config designated initializers when MSVC ICE is fixed
    // https://developercommunity.visualstudio.com/content/problem/1238876/fatal-error-c1001-ice-with-ehsc.html
    http_Config http;
    int max_age = 900;

    bool Validate() const;

    const char *GetInstanceFileName(const char *key, Allocator *alloc) const;

    BlockAllocator str_alloc;
};

bool LoadConfig(StreamReader *st, DomainConfig *out_config);
bool LoadConfig(const char *filename, DomainConfig *out_config);

class DomainHolder {
    struct InstanceGuard {
        // Keep first!
        InstanceHolder instance;

        std::atomic_int refcount {0};
        bool valid = true;
        bool reload = false;

        InstanceHolder *Ref()
        {
            refcount++;
            return &instance;
        }

        void Unref()
        {
            refcount--;
        }

        friend class DomainHolder;
    };

    bool synced = true;

    std::shared_mutex mutex;
    HeapArray<InstanceGuard *> instances;
    HashMap<Span<const char>, InstanceGuard *> instances_map;

public:
    DomainConfig config = {};
    sq_Database db;

    ~DomainHolder() { Close(); }

    bool Open(const char *filename);
    void Close();

    // Can be restarted (for debug builds)
    void InitAssets();

    bool IsSynced() const { return synced; }
    bool Sync();

    InstanceHolder *Ref(Span<const char> key, bool *out_reload = nullptr);
    void Unref(InstanceHolder *instance);
    void MarkForReload(InstanceHolder *instance);
};

bool MigrateDomain(sq_Database *db, const char *instances_directory);
bool MigrateDomain(const DomainConfig &config);

}
