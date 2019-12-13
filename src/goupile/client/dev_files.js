// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

let dev_files = new function() {
    let self = this;

    let remote = false;

    let actions;
    let user_actions = {};

    this.runFiles = async function() {
        actions = await vfs.status(remote);

        // Show locally deleted files last
        actions.sort((action1, action2) => (!!action2.local - !!action1.local) ||
                                           util.compareValues(action1.path, action2.path));

        // Overwrite with user actions (if any)
        for (let action of actions)
            action.type = user_actions[action.path] || action.type;

        renderActions();
    };

    function renderActions() {
        let enable_sync = actions.some(action => action.type !== 'noop') &&
                          !actions.some(action => action.type === 'conflict');

        render(html`
            <table class="sync_table">
                <thead><tr>
                    <th style="width: 0.7em;"></th>
                    <th>Fichier</th>
                    <th style="width: 5em;"></th>

                    ${remote ? html`
                        <th style="width: 2.8em;"></th>
                        <th>Distant</th>
                        <th style="width: 5em;"></th>
                    ` : ''}
                </tr></thead>

                <tbody>${actions.map(action => {
                    if (action.local || (remote && action.remote)) {
                        return html`<tr>
                            <td>${action.local ?
                                html`<a href="#" @click=${e => { showDeleteDialog(e, action.path); e.preventDefault(); }}>x</a>` : ''}</td>
                            <td class=${action.type == 'pull' ? 'sync_path overwrite' : 'sync_path'}>${action.local ? action.path : ''}</td>
                            <td class="sync_size">${action.local ? util.formatDiskSize(action.local.size) : ''}</td>

                            ${remote ? html`
                                <td class="sync_actions">
                                    <a href="#" class=${action.type !== 'pull' ? 'gray' : ''} @click=${e => toggleAction(action, 'pull')}>&lt;</a>
                                    <a href="#" class=${action.type !== 'noop' ? 'gray' : ''} @click=${e => toggleAction(action, 'noop')}>${action.type === 'conflict' ? '?' : '='}</a>
                                    <a href="#" class=${action.type !== 'push' ? 'gray' : ''} @click=${e => toggleAction(action, 'push')}>&gt;</a>
                                </td>
                                <td class=${action.type == 'push' ? 'sync_path overwrite' : 'sync_path'}>${action.remote ? action.path : ''}</td>
                                <td class="sync_size">${action.remote ? util.formatDiskSize(action.remote.size) : ''}</td>
                            ` : ''}
                        `;
                    } else {
                        // If remote is true, geting here means that nody the file because it was
                        // deleted on both sides independently. We need to act on it anyway in
                        // order to clean up any remaining local state about this file.
                        return '';
                    }
                })}</tbody>
            </table>

            <div id="dev_tools" class="gp_toolbar">
                <button @click=${showCreateDialog}>Ajouter</button>
                <div style="flex: 1;"></div>
                ${remote ?
                    html`<button ?disabled=${!enable_sync} @click=${showSyncDialog}>Synchroniser</button>` : ''}
                <button class=${remote ? 'active' : ''} @click=${toggleRemote}>Distant</button>
            </div>
        `, document.querySelector('#dev_files'));
    }

    function showCreateDialog(e, path, blob) {
        goupile.popup(e, page => {
            let blob = page.file('file', 'Fichier :', {mandatory: true});

            let default_path = blob.value ? `/app/${blob.value.name}` : null;
            let path = page.text('path', 'Chemin :', {placeholder: default_path});
            if (!path.value)
                path.value = default_path;

            if (path.value) {
                if (!path.value.match(/^\/app\/./)) {
                    path.error('Le chemin doit commencer par \'/app/\'');
                } else if (!path.value.match(/^\/app(\/[A-Za-z0-9_\.]+)+$/)) {
                    path.error('Allowed path characters: a-z, _, 0-9 and / (not at the end)');
                } else if (path.value.includes('/../') || path.value.endsWith('/..')) {
                    path.error('Le chemin ne doit pas contenir de composants \'..\'');
                } else if (actions.some(action => action.path === path.value && action.local)) {
                    path.error('Ce chemin est déjà utilisé');
                }
            }

            page.submitHandler = async () => {
                page.close();

                // FIXME: Need to refresh list of assets in dev
                let entry = new log.Entry;

                entry.progress('Enregistrement du fichier');
                try {
                    let file = vfs.create(path.value, blob.value || '');
                    await vfs.save(file);

                    entry.success('Fichier enregistré !');
                    app.go();
                } catch (err) {
                    entry.error(`Echec de l'enregistrement : ${err}`);
                }
            };
            page.buttons(page.buttons.std.ok_cancel('Créer'));
        });
    }

    function toggleRemote() {
        remote = !remote;
        self.runFiles();
    }

    function showSyncDialog(e) {
        goupile.popup(e, page => {
            page.output('Voulez-vous vraiment synchroniser les fichiers ?');

            page.submitHandler = async () => {
                page.close();

                await syncFiles();
                app.go();
            };
            page.buttons(page.buttons.std.ok_cancel('Synchroniser'));
        });
    }

    function showDeleteDialog(e, path) {
        goupile.popup(e, page => {
            page.output(`Voulez-vous vraiment supprimer '${path}' ?`);

            page.submitHandler = async () => {
                page.close();

                // FIXME: Need to refresh list of assets in dev
                await vfs.delete(path);
                app.go();
            };
            page.buttons(page.buttons.std.ok_cancel('Supprimer'));
        });
    }

    function toggleAction(action, type) {
        action.type = type;
        user_actions[action.path] = action.type;

        renderActions();
    }

    async function syncFiles() {
        let entry = new log.Entry;
        entry.progress('Synchronisation en cours');

        try {
            await vfs.sync(actions);
            entry.success('Synchronisation terminée !');
        } catch (err) {
            entry.error(err);
        }

        user_actions = {};
        await self.runFiles();
    }
};
