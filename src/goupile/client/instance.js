// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

function InstanceController() {
    let self = this;

    let app;

    let route = {
        form: null,
        page: null,
        ulid: null,
        version: null
    };

    let code_cache = new LruMap(4);
    let page_code;
    let page_div = document.createElement('div');

    let form_meta;
    let form_chain;
    let form_state;

    let editor_el;
    let editor_ace;
    let editor_filename;
    let editor_buffers = new LruMap(32);

    let ignore_editor_change = false;
    let ignore_editor_scroll = false;
    let ignore_page_scroll = false;

    let data_form;
    let data_rows;

    let head_length = Number.MAX_SAFE_INTEGER;
    let develop = false;
    let error_entries = {};

    this.start = async function() {
        initUI();
        await initApp();

        self.go(null, window.location.href).catch(err => {
            log.error(err);

            // Fall back to home page
            self.go(null, ENV.base_url);
        });
    };

    this.hasUnsavedData = function() {
        return form_state != null && form_state.hasChanged();
    };

    async function initApp() {
        let code = await fetchCode('main.js');

        try {
            let new_app = new ApplicationInfo;
            let builder = new ApplicationBuilder(new_app);

            runUserCode('Application', code, {
                app: builder
            });
            if (!new_app.pages.size)
                throw new Error('Main script does not define any page');

            new_app.home = new_app.pages.values().next().value;
            app = util.deepFreeze(new_app);
        } catch (err) {
            if (app == null) {
                let new_app = new ApplicationInfo;
                let builder = new ApplicationBuilder(new_app);

                // For simplicity, a lot of code assumes at least one page exists
                builder.page("default", "Page par défaut");

                new_app.home = new_app.pages.values().next().value;
                app = util.deepFreeze(new_app);
            }
        }

        if (app.head != null) {
            let template = document.createElement('template');
            render(app.head, template);

            // Clear previous changes
            for (let i = document.head.children.length - 1; i >= head_length; i--)
                document.head.removeChild(document.head.children[i]);
            head_length = document.head.children.length;

            for (let child of template.children)
                document.head.appendChild(child);
        }
    }

    function initUI() {
        document.documentElement.className = 'instance';

        ui.setMenu(renderMenu);
        ui.createPanel('editor', 0, false, renderEditor);
        ui.createPanel('data', 0, true, renderData);
        ui.createPanel('page', 1, false, renderPage);
    };

    function renderMenu() {
        return html`
            <button class="icon" style="background-position-y: calc(-538px + 1.2em);"
                    @click=${e => self.go(e, ENV.base_url)}>${ENV.title}</button>
            ${goupile.hasPermission('develop') ? html`
                <button class=${'icon' + (ui.isPanelEnabled('editor') ? ' active' : '')}
                        style="background-position-y: calc(-230px + 1.2em);"
                        @click=${ui.wrapAction(e => togglePanel(e, 'editor'))}>Code</button>
            ` : ''}
            <button class=${'icon' + (ui.isPanelEnabled('data') ? ' active' : '')}
                    style="background-position-y: calc(-274px + 1.2em);"
                    @click=${ui.wrapAction(e => togglePanel(e, 'data'))}>Suivi</button>
            <button class=${'icon' + (ui.isPanelEnabled('page') ? ' active' : '')}
                    style="background-position-y: calc(-318px + 1.2em);"
                    @click=${ui.wrapAction(e => togglePanel(e, 'page'))}>Formulaire</button>

            <div style="flex: 1; min-width: 15px;"></div>
            ${util.mapRange(0, route.form.parents.length, idx => {
                let form = route.form.parents[route.form.parents.length - idx - 1];
                return renderFormMenuDrop(form);
            })}
            ${renderFormMenuDrop(route.form)}
            ${route.form.menu.length > 1 ?
                html`<button class="active" @click=${ui.wrapAction(e => self.go(e, route.page.url))}>${route.page.title}</button>` : ''}
            <div style="flex: 1; min-width: 15px;"></div>

            <div class="drop right">
                <button class="icon" style="background-position-y: calc(-494px + 1.2em)">${profile.username}</button>
                <div>
                    <button @click=${ui.wrapAction(goupile.logout)}>Se déconnecter</button>
                </div>
            </div>
        `;
    }

    function renderFormMenuDrop(form) {
        if (form.menu.length > 1) {
            return html`
                <div class="drop">
                    <button>${form.title}</button>
                    <div>
                        ${util.map(form.menu, item => {
                            if (item.type === 'page') {
                                let page = item.page;
                                return html`<button ?disabled=${!isPageEnabled(page, form_chain[0])}
                                                    class=${page === route.page ? 'active' : ''}
                                                    @click=${ui.wrapAction(e => self.go(e, page.url))}>${page.title}</button>`;
                            } else if (item.type === 'form') {
                                let form = item.form;
                                return html`<button ?disabled=${!isFormEnabled(form, form_chain[0])}
                                                    class=${form === route.form || route.form.parents.some(parent => form === parent) ? 'active' : ''}
                                                    @click=${ui.wrapAction(e => self.go(e, form.url))}>${form.title}</button>`;
                            }
                        })}
                    </div>
                </div>
            `;
        } else {
            return html`<button ?disabled=${!isFormEnabled(form, form_chain[0])}
                                class=${form === route.form || route.form.parents.some(parent => form === parent) ? 'active' : ''}
                                @click=${ui.wrapAction(e => self.go(e, form.url))}>${form.title}</button>`;
        }
    }

    async function togglePanel(e, key) {
        let enable = !ui.isPanelEnabled(key);
        ui.setPanelState(key, enable);

        await self.run();

        if (enable && key === 'page') {
            syncFormScroll();
            syncFormHighlight(true);
        } else if (key === 'editor') {
            if (enable)
                syncEditorScroll();
            syncFormHighlight(false);
        }
    }

    function renderEditor() {
        let tabs = [];
        tabs.push({
            title: 'Application',
            filename: 'main.js'
        });
        tabs.push({
            title: 'Formulaire',
            filename: route.page.filename
        });

        // Ask ACE to adjust if needed, it needs to happen after the render
        setTimeout(() => editor_ace.resize(false), 0);

        return html`
            <div style="--menu_color: #383936;">
                <div class="ui_toolbar">
                    ${tabs.map(tab => html`<button class=${editor_filename === tab.filename ? 'active' : ''}
                                                   @click=${ui.wrapAction(e => toggleEditorFile(tab.filename))}>${tab.title}</button>`)}
                    <div style="flex: 1;"></div>
                    <button @click=${ui.wrapAction(runDeployDialog)}>Publier</button>
                </div>

                ${editor_el}
            </div>
        `;
    }

    function renderData() {
        let columns = data_form.pages.size + data_form.children.size;

        return html`
            <div class="padded">
                <div class="ui_quick">
                    ${data_rows.length || 'Aucune'} ${data_rows.length > 1 ? 'lignes' : 'ligne'}
                    <div style="flex: 1;"></div>
                    ${ENV.backup_key != null ? html`
                        <a @click=${ui.wrapAction(e => backupClientData('file'))}>Faire une sauvegarde chiffrée</a>
                        <div style="flex: 1;"></div>
                    ` : ''}
                    <a @click=${ui.wrapAction(e => { data_rows = null; return self.run(); })}>Rafraichir</a>
                </div>

                <table class="ui_table" id="ins_data">
                    <colgroup>
                        <col style="width: 2em;"/>
                        <col style="width: 160px;"/>
                        ${util.mapRange(0, columns, () => html`<col/>`)}
                    </colgroup>

                    <thead>
                        <tr>
                            <th></th>
                            <th></th>
                            ${util.map(data_form.pages.values(), page => html`<th>${page.title}</th>`)}
                            ${util.map(data_form.children.values(), child_form => html`<th>${child_form.title}</th>`)}
                        </tr>
                    </thead>

                    <tbody>
                        ${data_rows.map(row => {
                            let active = form_chain.some(meta => meta.ulid === row.ulid);

                            return html`
                                <tr class=${active ? 'active' : ''}>
                                    <td>
                                        <a @click=${ui.wrapAction(e => runDeleteRecordDialog(e, row.ulid))}>✕</a>
                                    </td>
                                    <td class=${row.hid == null ? 'missing' : ''}>${row.hid != null ? row.hid : 'NA'}</td>
                                    ${util.map(data_form.pages.values(), page => {
                                        let url = page.url + `/${row.ulid}`;

                                        if (row.status.has(page.key)) {
                                            return html`<td class="saved"><a href=${url}
                                                                             @click=${e => ui.setPanelState('page', true, false)}>Enregistré</a></td>`;
                                        } else {
                                            return html`<td class="missing"><a href=${url}
                                                                               @click=${e => ui.setPanelState('page', true, false)}>Non rempli</a></td>`;
                                        }
                                    })}
                                    ${util.map(data_form.children.values(), child_form => {
                                        if (row.status.has(child_form.key)) {
                                            let child = row.children.get(child_form.key);
                                            let url = child_form.url + `/${child.ulid}`;

                                            return html`
                                                <td class="saved">
                                                    <a href=${url} @click=${e => ui.setPanelState('page', true, false)}>Enregistré</a>
                                                </td>
                                            `;
                                        } else {
                                            let url = child_form.url + `/${row.ulid}`;

                                            return html`
                                                <td class="missing">
                                                    <a href=${url} @click=${e => ui.setPanelState('page', true, false)}>Non rempli</a>
                                                </td>
                                            `;
                                        }
                                    })}
                                </tr>
                            `;
                        })}
                        ${!data_rows.length ? html`<tr><td colspan=${2 + columns}>Aucune ligne à afficher</td></tr>` : ''}
                    </tbody>
                </table>

                <div class="ui_actions">
                    <button @click=${ui.wrapAction(goNewRecord)}>Créer un nouvel enregistrement</button>
                </div>
            </div>
        `;
    }

    function renderPage() {
        let readonly = (route.version < form_meta.fragments.length);

        let model = new FormModel;
        let builder = new FormBuilder(form_state, model, readonly);

        try {
            builder.pushOptions({});

            let meta = util.assignDeep({}, form_meta);
            runUserCode('Formulaire', page_code, {
                form: builder,
                values: form_state.values,
                meta: meta,
                nav: {
                    form: route.form,
                    page: route.page,
                    go: (url) => self.go(null, url)
                }
            });
            form_meta.hid = meta.hid;

            builder.popOptions({});

            if (model.hasErrors())
                builder.errorList();

            if (model.variables.length) {
                let enable_save = !model.hasErrors() && form_state.hasChanged();

                builder.action('Enregistrer', {disabled: !enable_save}, () => {
                    if (builder.triggerErrors())
                        return saveRecord();
                });
                builder.action('-');
                builder.action('Nouveau', {}, goNewRecord);
            }

            render(model.render(), page_div);
            page_div.classList.remove('disabled');
        } catch (err) {
            if (!page_div.children.length)
                render('Impossible de générer la page à cause d\'une erreur', page_div);
            page_div.classList.add('disabled');
        }

        return html`
            <div class="print" @scroll=${syncEditorScroll}}>
                <div id="ins_page">
                    <div class="ui_quick">
                        ${model.variables.length ? html`
                            ${!form_chain[0].version ? 'Nouvel enregistrement' : ''}
                            ${form_chain[0].version > 0 && form_chain[0].hid != null ? `Enregistrement : ${form_chain[0].hid}` : ''}
                            ${form_chain[0].version > 0 && form_chain[0].hid == null ? 'Enregistrement local' : ''}
                        `  : ''}
                        (<a @click=${e => window.print()}>imprimer</a>)
                        <div style="flex: 1;"></div>
                        ${route.version > 0 ? html`
                            ${route.version < form_meta.fragments.length ?
                                html`<span style="color: red;">Ancienne version du ${form_meta.mtime.toLocaleString()}</span>` : ''}
                            ${route.version === form_meta.fragments.length ? html`<span>Version actuelle</span>` : ''}
                            &nbsp;(<a @click=${ui.wrapAction(e => runTrailDialog(e, route.ulid))}>historique</a>)
                        ` : ''}
                    </div>

                    <form id="ins_form" @submit=${e => e.preventDefault()}>
                        ${page_div}
                    </form>
                </div>

                ${develop ? html`
                    <div style="flex: 1;"></div>
                    <div id="ins_notice">
                        Formulaires en développement<br/>
                        Publiez les avant d'enregistrer des données
                    </div>
                ` : ''}
            </div>
        `;
    }

    function runUserCode(title, code, arguments) {
        let entry = error_entries[title];
        if (entry == null) {
            entry = new log.Entry;
            error_entries[title] = entry;
        }

        try {
            let func = new Function(...Object.keys(arguments), code);
            func(...Object.values(arguments));

            entry.close();
        } catch (err) {
            let line = util.parseEvalErrorLine(err);
            let msg = `Erreur sur ${title}\n${line != null ? `Ligne ${line} : ` : ''}${err.message}`;

            entry.error(msg, -1);
            throw new Error(msg);
        }
    }

    function runTrailDialog(e, ulid) {
        return ui.runDialog(e, (d, resolve, reject) => {
            if (ulid !== route.ulid)
                reject();

            d.output(html`
                <table class="ui_table">
                    <tbody>
                        ${util.mapRange(0, form_meta.fragments.length, idx => {
                            let version = form_meta.fragments.length - idx;
                            let fragment = form_meta.fragments[version - 1];

                            let page = app.pages.get(fragment.page) || route.page;
                            let url = page.url + `/${ulid}@${version}`;

                            return html`
                                <tr class=${version === route.version ? 'active' : ''}>
                                    <td><a href=${url}>🔍\uFE0E</a></td>
                                    <td>${fragment.user}</td>
                                    <td>${fragment.mtime.toLocaleString()}</td>
                                </tr>
                            `;
                        })}
                    </tbody>
                </table>
            `);
        });
    }

    function goNewRecord(e) {
        if (form_chain.length > 1) {
            let url = route.form.parents[route.form.parents.length - 1].url + '/new';
            return self.go(e, url);
        } else {
            let url = route.form.url + '/new';
            return self.go(e, url);
        }
    }

    async function saveRecord() {
        if (develop)
            throw new Error('Enregistrement refusé : formulaire non publié');

        let progress = log.progress('Enregistrement en cours');

        try {
            enablePersistence();

            let page = route.page;
            let meta = form_meta;
            let values = Object.assign({}, form_state.values);

            // Can't store undefined in IndexedDB...
            for (let key in values) {
                if (values[key] === undefined)
                    values[key] = null;
            }

            let key = `${profile.userid}:${meta.ulid}`;
            let fragment = {
                type: 'save',
                user: profile.username,
                mtime: new Date,
                page: page.key,
                values: values
            };

            await db.transaction('rw', ['rec_records'], async t => {
                let obj = await t.load('rec_records', key);

                let record;
                if (obj != null) {
                    record = await goupile.decryptLocal(obj.enc);
                    if (meta.hid != null)
                        record.hid = meta.hid;
                } else {
                    obj = {
                        fkey: `${profile.userid}/${page.form.key}`,
                        pkey: null,
                        enc: null
                    };
                    record = {
                        ulid: meta.ulid,
                        hid: meta.hid,
                        parent: null,
                        form: page.form.key,
                        fragments: []
                    };

                    if (meta.parent != null) {
                        obj.pkey = `${profile.userid}:${meta.parent.ulid}`;
                        record.parent = {
                            form: meta.parent.form.key,
                            ulid: meta.parent.ulid,
                            version: meta.parent.version
                        };
                    }
                }

                if (meta.version !== record.fragments.length)
                    throw new Error('Cannot overwrite old record fragment');
                record.fragments.push(fragment);

                obj.enc = await goupile.encryptLocal(record);
                await t.saveWithKey('rec_records', key, obj);

                if (record.parent != null) {
                    let child = {
                        form: record.form,
                        ulid: record.ulid,
                        version: record.fragments.length
                    };
                    await updateRecordParents(t, record.parent, child, 'save_child', fragment.mtime);
                }
            });

            progress.success('Enregistrement effectué');

            // XXX: Trigger reload in a better way...
            route.version = null;
            form_meta = null;
            form_state = null;
            data_rows = null;

            self.go(null, window.location.href);
        } catch (err) {
            progress.close();
            throw err;
        }
    }

    function runDeleteRecordDialog(e, ulid) {
        return ui.runConfirm(e, 'Voulez-vous vraiment supprimer cet enregistrement ?', 'Supprimer', async () => {
            let progress = log.progress('Suppression en cours');

            try {
                let key = `${profile.userid}:${ulid}`;
                let fragment = {
                    type: 'delete',
                    user: profile.username,
                    mtime: new Date
                };

                // XXX: Delete children (cascade)?
                await db.transaction('rw', ['rec_records'], async t => {
                    let obj = await t.load('rec_records', key);
                    if (obj == null)
                        throw new Error('Cet enregistrement est introuvable');

                    let record = await goupile.decryptLocal(obj.enc);

                    // Mark as deleted with special fragment
                    record.fragments.push(fragment);

                    obj.enc = await goupile.encryptLocal(record);
                    await t.saveWithKey('rec_records', key, obj);

                    if (record.parent != null) {
                        let child = {
                            form: record.form,
                            ulid: record.ulid,
                            version: record.fragments.length
                        };
                        await updateRecordParents(t, record.parent, child, 'delete_child', fragment.mtime);
                    }
                });

                progress.success('Suppression effectuée');

                data_rows = null;
                if (form_chain.some(meta => meta.ulid === ulid)) {
                    form_state = null;
                    goNewRecord(null);
                } else {
                    self.run();
                }
            } catch (err) {
                progress.close();
                throw err;
            }
        })
    }

    // Call in transaction!
    async function updateRecordParents(t, parent, child, type, mtime) {
        while (parent != null) {
            let parent_key = `${profile.userid}:${parent.ulid}`;
            let parent_obj = await t.load('rec_records', parent_key);
            let parent_record = await goupile.decryptLocal(parent_obj.enc);

            let parent_fragment = {
                type: type,
                user: profile.username,
                mtime: mtime,
                child: child
            };
            parent_record.fragments.push(parent_fragment);

            parent_obj.enc = await goupile.encryptLocal(parent_record);
            await t.saveWithKey('rec_records', parent_key, parent_obj);

            type = 'save_child';

            child = {
                form: parent_record.form,
                ulid: parent_record.ulid,
                version: parent_record.fragments.length
            };
            parent = parent_record.parent;
        }
    }

    function enablePersistence() {
        let storage_warning = 'Impossible d\'activer le stockage local persistant';

        if (navigator.storage && navigator.storage.persist) {
            navigator.storage.persist().then(granted => {
                if (granted) {
                    console.log('Stockage persistant activé');
                } else {
                    console.log(storage_warning);
                }
            });
        } else {
            console.log(storage_warning);
        }
    }

    async function syncEditor() {
        if (editor_el == null) {
            if (typeof ace === 'undefined')
                await net.loadScript(`${ENV.base_url}static/ace.js`);

            editor_el = document.createElement('div');
            editor_el.setAttribute('style', 'flex: 1;');
            editor_ace = ace.edit(editor_el);

            editor_ace.setTheme('ace/theme/merbivore_soft');
            editor_ace.setShowPrintMargin(false);
            editor_ace.setFontSize(13);
            editor_ace.setBehavioursEnabled(false);
            editor_ace.setOptions({
                scrollPastEnd: 1
            });
        }

        let buffer = editor_buffers.get(editor_filename);

        if (buffer == null) {
            let code = await fetchCode(editor_filename);

            let session = new ace.EditSession('', 'ace/mode/javascript');
            session.setOption('useWorker', false);
            session.setUseWrapMode(true);
            session.doc.setValue(code);
            session.setUndoManager(new ace.UndoManager());
            session.on('change', e => handleFileChange(editor_filename));

            session.on('changeScrollTop', () => {
                if (ui.isPanelEnabled('page'))
                    setTimeout(syncFormScroll, 0);
            });
            session.selection.on('changeSelection', () => {
                syncFormHighlight(true);
                ignore_editor_scroll = true;
            });

            buffer = {
                session: session,
                reload: false
            };
            editor_buffers.set(editor_filename, buffer);
        } else if (buffer.reload) {
            let code = await fetchCode(editor_filename);

            ignore_editor_change = true;
            buffer.session.doc.setValue(code);
            ignore_editor_change = false;

            buffer.reload = false;
        }

        editor_ace.setSession(buffer.session);
    }

    function toggleEditorFile(filename) {
        editor_filename = filename;
        return self.run();
    }

    async function handleFileChange(filename) {
        if (ignore_editor_change)
            return;

        let buffer = editor_buffers.get(filename);

        // Should never fail, but who knows..
        if (buffer != null) {
            let key = `${profile.userid}:${filename}`;
            let blob = new Blob([buffer.session.doc.getValue()]);
            let sha256 = await computeSha256(blob);

            await db.saveWithKey('fs_files', key, {
                filename: filename,
                size: blob.size,
                sha256: await computeSha256(blob),
                blob: blob
            });
        }

        self.run();
    }

    function syncFormScroll() {
        if (!ui.isPanelEnabled('editor') || !ui.isPanelEnabled('page'))
            return;
        if (ignore_editor_scroll) {
            ignore_editor_scroll = false;
            return;
        }

        try {
            let panel_el = document.querySelector('#ins_page').parentNode;
            let widget_els = panel_el.querySelectorAll('*[data-line]');

            let editor_line = editor_ace.getFirstVisibleRow() + 1;

            let prev_line;
            for (let i = 0; i < widget_els.length; i++) {
                let line = parseInt(widget_els[i].dataset.line, 10);

                if (line >= editor_line) {
                    if (!i) {
                        ignore_page_scroll = true;
                        panel_el.scrollTop = 0;
                    } else if (i === widget_els.length - 1) {
                        let top = computeRelativeTop(panel_el, widget_els[i]);

                        ignore_page_scroll = true;
                        panel_el.scrollTop = top;
                    } else {
                        let top1 = computeRelativeTop(panel_el, widget_els[i - 1]);
                        let top2 = computeRelativeTop(panel_el, widget_els[i]);
                        let frac = (editor_line - prev_line) / (line - prev_line);

                        ignore_page_scroll = true;
                        panel_el.scrollTop = top1 + frac * (top2 - top1);
                    }

                    break;
                }

                prev_line = line;
            }
        } catch (err) {
            // Meh, don't wreck the editor if for some reason we can't sync the
            // two views, this is not serious so just log it.
            console.log(err);
        }
    }

    function computeRelativeTop(parent, el) {
        let top = 0;
        while (el !== parent) {
            top += el.offsetTop;
            el = el.offsetParent;
        }
        return top;
    }

    function syncFormHighlight(scroll) {
        if (!ui.isPanelEnabled('page'))
            return;

        try {
            let panel_el = document.querySelector('#ins_page').parentNode;
            let widget_els = panel_el.querySelectorAll('*[data-line]');

            if (ui.isPanelEnabled('editor') && widget_els.length) {
                let selection = editor_ace.session.selection;
                let editor_lines = [
                    selection.getRange().start.row + 1,
                    selection.getRange().end.row + 1
                ];

                let highlight_first;
                let highlight_last;
                for (let i = 0;; i++) {
                    let line = parseInt(widget_els[i].dataset.line, 10);

                    if (line > editor_lines[0]) {
                        if (i > 0)
                            highlight_first = i - 1;
                        break;
                    }

                    if (i >= widget_els.length - 1) {
                        highlight_first = i;
                        break;
                    }
                }
                if (highlight_first != null) {
                    highlight_last = highlight_first;

                    while (highlight_last < widget_els.length) {
                        let line = parseInt(widget_els[highlight_last].dataset.line, 10);
                        if (line > editor_lines[1])
                            break;
                        highlight_last++;
                    }
                    highlight_last--;
                }

                for (let i = 0; i < widget_els.length; i++)
                    widget_els[i].classList.toggle('ins_highlight', i >= highlight_first && i <= highlight_last);

                // Make sure widget is in viewport
                if (scroll && highlight_first != null &&
                              highlight_last === highlight_first) {
                    let el = widget_els[highlight_first];
                    let rect = el.getBoundingClientRect();

                    if (rect.top < 0) {
                        ignore_page_scroll = true;
                        panel_el.scrollTop += rect.top - 50;
                    } else if (rect.bottom >= window.innerHeight) {
                        ignore_page_scroll = true;
                        panel_el.scrollTop += rect.bottom - window.innerHeight + 30;
                    }
                }
            } else {
                for (let el of widget_els)
                    el.classList.remove('ins_highlight');
            }
        } catch (err) {
            // Meh, don't wreck the editor if for some reason we can't sync the
            // two views, this is not serious so just log it.
            console.log(err);
        }
    }

    function syncEditorScroll() {
        if (!ui.isPanelEnabled('editor') || !ui.isPanelEnabled('page'))
            return;
        if (ignore_page_scroll) {
            ignore_page_scroll = false;
            return;
        }

        try {
            let panel_el = document.querySelector('#ins_page').parentNode;
            let widget_els = panel_el.querySelectorAll('*[data-line]');

            let prev_top;
            let prev_line;
            for (let i = 0; i < widget_els.length; i++) {
                let el = widget_els[i];

                let top = el.getBoundingClientRect().top;
                let line = parseInt(el.dataset.line, 10);

                if (top >= 0) {
                    if (!i) {
                        ignore_editor_scroll = true;
                        editor_ace.renderer.scrollToLine(0);
                    } else {
                        let frac = -prev_top / (top - prev_top);
                        let line2 = Math.floor(prev_line + frac * (line - prev_line));

                        ignore_editor_scroll = true;
                        editor_ace.renderer.scrollToLine(line2);
                    }

                    break;
                }

                prev_top = top;
                prev_line = line;
            }
        } catch (err) {
            // Meh, don't wreck anything if for some reason we can't sync the
            // two views, this is not serious so just log it.
            console.log(err);
        }
    }

    // Javascript Blob/File API sucks, plain and simple
    async function computeSha256(blob) {
        let sha256 = new Sha256;

        for (let offset = 0; offset < blob.size; offset += 65536) {
            let piece = blob.slice(offset, offset + 65536);
            let buf = await new Promise((resolve, reject) => {
                let reader = new FileReader;

                reader.onload = e => resolve(e.target.result);
                reader.onerror = e => {
                    reader.abort();
                    reject(new Error(e.target.error));
                };

                reader.readAsArrayBuffer(piece);
            });

            sha256.update(buf);
        }

        return sha256.finalize();
    }

    // XXX: Broken refresh, etc
    async function runDeployDialog(e) {
        let actions = await computeDeployActions();
        let modifications = actions.reduce((acc, action) => acc + (action.type !== 'noop'), 0);

        return ui.runDialog(e, (d, resolve, reject) => {
            d.output(html`
                <div class="ui_quick">
                    ${modifications || 'Aucune'} ${modifications.length > 1 ? 'modifications' : 'modification'} à effectuer
                    <div style="flex: 1;"></div>
                    <a @click=${ui.wrapAction(e => { actions = null; return self.run(); })}>Rafraichir</a>
                </div>

                <table class="ui_table ins_deploy">
                    <colgroup>
                        <col style="width: 2.4em;"/>
                        <col/>
                        <col style="width: 4.6em;"/>
                        <col style="width: 8em;"/>
                        <col/>
                        <col style="width: 4.6em;"/>
                    </colgroup>

                    <tbody>
                        ${actions.map(action => {
                            let local_cls = 'path';
                            if (action.type === 'pull') {
                                local_cls += action.remote_sha256 ? ' overwrite' : ' overwrite delete';
                            } else if (action.local_sha256 == null) {
                                local_cls += ' virtual';
                            }

                            let remote_cls = 'path';
                            if (action.type === 'push') {
                                remote_cls += action.local_sha256 ? ' overwrite' : ' overwrite delete';
                            } else if (action.remote_sha256 == null) {
                                remote_cls += ' none';
                            }

                            return html`
                                <tr class=${action.type === 'conflict' ? 'conflict' : ''}>
                                    <td>
                                        <a @click=${ui.wrapAction(e => runDeleteFileDialog(e, action.filename))}>x</a>
                                    </td>
                                    <td class=${local_cls}>${action.filename}</td>
                                    <td class="size">${action.local_sha256 ? util.formatDiskSize(action.local_size) : ''}</td>

                                    <td class="actions">
                                        <a class=${action.type === 'pull' ? 'active' : (action.local_sha256 == null || action.local_sha256 === action.remote_sha256 ? 'disabled' : '')}
                                           @click=${e => { action.type = 'pull'; d.restart(); }}>&lt;</a>
                                        <a class=${action.type === 'push' ? 'active' : (action.local_sha256 == null || action.local_sha256 === action.remote_sha256 ? 'disabled' : '')}
                                           @click=${e => { action.type = 'push'; d.restart(); }}>&gt;</a>
                                    </td>

                                    <td class=${remote_cls}>${action.filename}</td>
                                    <td class="size">
                                        ${action.type === 'conflict' ? html`<span class="conflict" title="Modifications en conflit">⚠\uFE0E</span>&nbsp;` : ''}
                                        ${action.remote_sha256 ? util.formatDiskSize(action.remote_size) : ''}
                                    </td>
                                </tr>
                            `;
                        })}
                    </tbody>
                </table>

                <div class="ui_quick">
                    <div style="flex: 1;"></div>
                    <a @click=${ui.wrapAction(runAddFileDialog)}>Ajouter un fichier</a>
                </div>
            `);

            d.action('Publier', {disabled: !d.isValid()}, async () => {
                await deploy(actions);

                resolve();
                self.run();
            });
        });
    }

    async function computeDeployActions() {
        let range = IDBKeyRange.bound(profile.userid + ':', profile.userid + '`', false, true);
        let [local_files, remote_files] = await Promise.all([
            db.loadAll('fs_files', range),
            net.fetchJson(`${ENV.base_url}api/files/list`)
        ]);

        let local_map = util.arrayToObject(local_files, file => file.filename);
        // XXX: let sync_map = util.arrayToObject(sync_files, file => file.filename);
        let remote_map = util.arrayToObject(remote_files, file => file.filename);

        let actions = [];

        // Handle local files
        for (let local_file of local_files) {
            let remote_file = remote_map[local_file.filename];
            let sync_file = remote_file; // XXX: sync_map[local_file.filename];

            if (remote_file != null) {
                if (local_file.sha256 != null) {
                    if (local_file.sha256 === remote_file.sha256) {
                        actions.push(makeDeployAction(local_file.filename, local_file, remote_file, 'noop'));
                    } else if (sync_file != null && sync_file.sha256 === remote_file.sha256) {
                        actions.push(makeDeployAction(local_file.filename, local_file, remote_file, 'push'));
                    } else {
                        actions.push(makeDeployAction(local_file.filename, local_file, remote_file, 'conflict'));
                    }
                } else {
                    actions.push(makeDeployAction(local_file.filename, local_file, remote_file, 'noop'));
                }
            } else {
                if (sync_file == null) {
                    actions.push(makeDeployAction(local_file.filename, local_file, null, 'push'));
                } else if (sync_file.sha256 === local_file.sha256) {
                    actions.push(makeDeployAction(local_file.filename, local_file, null, 'pull'));
                } else {
                    actions.push(makeDeployAction(local_file.filename, local_file, null, 'conflict'));
                }
            }
        }

        // Pull remote-only files
        {
            // let new_sync_files = [];

            for (let remote_file of remote_files) {
                if (local_map[remote_file.filename] == null) {
                    actions.push(makeDeployAction(remote_file.filename, null, remote_file, 'noop'));

                    //new_sync_files.push(remote_file);
                }
            }

            // await db.saveAll('fs_sync', new_sync_files);
        }

        return actions;
    };

    function makeDeployAction(filename, local, remote, type) {
        let action = {
            filename: filename,
            type: type
        };

        if (local != null) {
            action.local_size = local.size;
            action.local_sha256 = local.sha256;
        }
        if (remote != null) {
            action.remote_size = remote.size;
            action.remote_sha256 = remote.sha256;
        }

        return action;
    }

    function runAddFileDialog(e) {
        return ui.runDialog(e, (d, resolve, reject) => {
            let file = d.file('*file', 'Fichier :');
            let filename = d.text('*filename', 'Chemin :', {value: file.value ? file.value.name : null});

            if (filename.value) {
                if (!filename.value.match(/^[A-Za-z0-9_\.]+(\/[A-Za-z0-9_\.]+)*$/))
                    filename.error('Caractères autorisés: a-z, 0-9, \'_\', \'.\' et \'/\' (pas au début ou à la fin)');
                if (filename.value.includes('/../') || filename.value.endsWith('/..'))
                    filename.error('Le chemin ne doit pas contenir de composants \'..\'');
            }

            d.action('Créer', {disabled: !d.isValid()}, async () => {
                let progress = new log.Entry;

                progress.progress('Enregistrement du fichier');
                try {
                    let key = `${profile.userid}:${filename.value}`;
                    await db.saveWithKey('fs_files', key, {
                        filename: filename.value,
                        size: file.value.size,
                        sha256: await computeSha256(filename.value),
                        blob: file.value
                    })

                    let buffer = editor_buffers.get(file.filename);
                    if (buffer != null)
                        buffer.reload = true;

                    progress.success('Fichier enregistré');
                    resolve();

                    self.run();
                } catch (err) {
                    progress.close();
                    reject(err);
                }
            });
        });
    }

    function runDeleteFileDialog(e, filename) {
        log.error('NOT IMPLEMENTED');
    }

    async function deploy(actions) { // XXX: reentrancy
        let progress = log.progress('Publication en cours');

        try {
            if (actions.some(action => action.type == 'conflict'))
                throw new Error('Conflits non résolus');

            for (let i = 0; i < actions.length; i += 10) {
                let p = actions.slice(i, i + 10).map(async action => {
                    switch (action.type) {
                        case 'push': {
                            let url = util.pasteURL(`${ENV.base_url}files/${action.filename}`, {sha256: action.remote_sha256 || ''});

                            if (action.local_sha256 != null) {
                                let key = `${profile.userid}:${action.filename}`;
                                let file = await db.load('fs_files', key);

                                let response = await net.fetch(url, {method: 'PUT', body: file.blob});
                                if (!response.ok) {
                                    let err = (await response.text()).trim();
                                    throw new Error(err);
                                }

                                await db.delete('fs_files', key);
                            } else {
                                let response = await net.fetch(url, {method: 'DELETE'});
                                if (!response.ok && response.status !== 404) {
                                    let err = (await response.text()).trim();
                                    throw new Error(err);
                                }
                            }
                        } break;

                        case 'noop':
                        case 'pull': {
                            let key = `${profile.userid}:${action.filename}`;
                            await db.delete('fs_files', key);

                            let buffer = editor_buffers.get(action.filename);
                            if (buffer != null)
                                buffer.reload = true;
                        } break;
                    }
                });
                await Promise.all(p);
            }

            progress.success('Publication effectuée');
        } catch (err) {
            progress.close();
            throw err;
        } finally {
            code_cache.clear();
        }
    };

    this.go = async function(e = null, url = null, push_history = true) {
        await goupile.syncProfile();
        if (!goupile.isAuthorized()) {
            await goupile.runLogin();

            if (net.isOnline() && ENV.backup_key != null)
                await backupClientData('server');
        }

        let new_route = Object.assign({}, route);
        let new_meta = form_meta;
        let prev_meta = form_meta;
        let new_chain = form_chain;
        let new_state = form_state;

        // Fix up URL
        if (!url.endsWith('/'))
            url += '/';
        url = new URL(url, window.location.href);
        if (url.pathname === ENV.base_url)
            url = new URL(app.home.url, window.location.href);

        // Goodbye!
        if (!url.pathname.startsWith(`${ENV.base_url}main/`))
            window.location.href = url.href;

        // Parse new URL
        {
            let path = url.pathname.substr(ENV.base_url.length + 5);
            let [key, what] = path.split('/').map(str => str.trim());

            // Support shorthand URLs: /main/<ULID>(@<VERSION>)
            if (key && key.match(/^[A-Z0-9]{26}(@[0-9]+)?$/)) {
                what = key;
                key = app.home.key;
            }

            // Find page information
            new_route.page = app.pages.get(key);
            if (new_route.page == null) {
                log.error(`La page '${key}' n'existe pas`);
                new_route.page = app.home;
            }
            new_route.form = new_route.page.form;

            let [ulid, version] = what ? what.split('@') : [null, null];

            // Popping history
            if (!ulid && !push_history)
                ulid = 'new';

            // Deal with ULID
            if (ulid && ulid !== new_route.ulid) {
                if (ulid === 'new')
                    ulid = null;

                new_route.ulid = ulid;
                new_route.version = null;
            }

            // And with version!
            if (version) {
                version = version.trim();

                if (version.match(/^[0-9]+$/)) {
                    new_route.version = parseInt(version, 10);
                } else {
                    log.error('L\'indicateur de version n\'est pas un nombre');
                    new_route.version = null;
                }
            }
        }

        // Load record if needed
        if (new_meta != null && (new_route.ulid !== new_meta.ulid ||
                                 new_route.version !== new_meta.version))
            new_meta = null;
        if (new_meta == null && new_route.ulid != null) {
            new_meta = await loadRecord(new_route.ulid, new_route.version);

            new_state = new FormState(new_meta.values);
            new_state.changeHandler = handleStateChange;
        }

        // Go to parent or child automatically
        if (new_meta != null && new_meta.form !== new_route.form) {
            let path = computePath(new_meta.form, new_route.form);

            if (path != null) {
                for (let form of path.up) {
                    new_meta = await loadRecord(new_meta.parent.ulid, null);

                    if (new_meta.form !== form)
                        throw new Error('Saut impossible en raison d\'un changement de schéma');
                }

                for (let i = 0; i < path.down.length; i++) {
                    let form = path.down[i];
                    let child = new_meta.children.get(form.key);

                    if (child != null) {
                        new_meta = await loadRecord(child.ulid, child.version);

                        if (new_meta.form !== form)
                            throw new Error('Saut impossible en raison d\'un changement de schéma');
                    } else {
                        // Save fake intermediate records if needed
                        if (!new_meta.version) {
                            let key = `${profile.userid}:${new_meta.ulid}`;

                            let obj = {
                                fkey: `${profile.userid}/${new_meta.form.key}`,
                                pkey: null,
                                enc: null
                            };
                            let record = {
                                ulid: new_meta.ulid,
                                hid: new_meta.hid,
                                parent: null,
                                form: new_meta.form.key,
                                fragments: [{
                                    type: 'fake',
                                    user: profile.username,
                                    mtime: new Date
                                }]
                            };

                            if (new_meta.parent != null) {
                                obj.pkey = `${profile.userid}:${new_meta.parent.ulid}`;
                                record.parent = {
                                    form: new_meta.parent.form.key,
                                    ulid: new_meta.parent.ulid,
                                    version: new_meta.parent.version
                                };
                            }

                            obj.enc = await goupile.encryptLocal(record);
                            await db.saveWithKey('rec_records', key, obj);
                        }

                        prev_meta = new_meta;
                        new_meta = createRecord(form, prev_meta);
                    }
                }

                if (new_meta != null) {
                    new_route.ulid = new_meta.ulid;
                    new_route.version = new_meta.version;
                    new_state = new FormState(new_meta.values);
                    new_state.changeHandler = handleStateChange;
                }
            } else {
                new_route.ulid = null;
                new_meta = null;
            }
        }

        // Create new record if needed
        if (new_route.ulid == null || new_meta == null) {
            new_meta = createRecord(new_route.form, null);

            new_route.ulid = new_meta.ulid;
            new_route.version = new_meta.version;
            new_state = new FormState(new_meta.values);
            new_state.changeHandler = handleStateChange;
        }

        // Load record parents
        if (new_chain == null || new_chain[new_chain.length - 1] !== new_meta) {
            new_chain = [new_meta];

            let meta = new_meta;
            while (meta.parent != null) {
                let parent_meta = await loadRecord(meta.parent.ulid);

                new_chain.push(parent_meta);
                meta.parent = parent_meta;

                meta = parent_meta;
            }

            new_chain.reverse();
        }

        if (!isPageEnabled(new_route.page, new_chain[0]))
            throw new Error('Cette page n\'est pas accessible pour le moment');

        // Confirm dangerous actions (at risk of data loss)
        if (form_state != null && form_state.hasChanged() && new_meta !== form_meta) {
            try {
                // XXX: Improve message if going to child form
                await ui.runConfirm(e, "Si vous continuez, vous perdrez les modifications en cours. Voulez-vous continuer ?",
                                       "Continuer", () => {});
            } catch (err) {
                // If we're popping state, this will fuck up navigation history but we can't
                // refuse popstate events. History mess is better than data loss.
                return self.run();
            }
        }

        // Help the user fill new forms
        if (form_meta != null && new_meta.ulid !== form_meta.ulid) {
            setTimeout(() => {
                let el = document.querySelector('#ins_page');
                if (el != null)
                    el.parentNode.scrollTop = 0;
            });

            ui.setPanelState('page', true, false);
        } else if (form_meta == null && (new_meta.version > 0 ||
                                         new_meta.parent != null)) {
            ui.setPanelState('page', true, false);
        }

        // Commit!
        route = new_route;
        route.version = new_meta.version;
        form_meta = new_meta;
        form_chain = new_chain;
        form_state = new_state;

        document.title = `${route.page.title} — ${ENV.title}`;
        await self.run(push_history);
    };
    this.go = util.serializeAsync(this.go);

    async function handleStateChange() {
        await self.run();

        // Highlight might need to change (conditions, etc.)
        if (ui.isPanelEnabled('editor'))
            syncFormHighlight(false);
    }

    function createRecord(form, parent_meta) {
        let meta = {
            form: form,
            ulid: util.makeULID(),
            hid: null,
            version: 0,
            parent: null,
            children: new Map,
            mtime: null,
            fragments: [],
            status: new Set,
            values: {}
        };

        if (form.parents.length) {
            if (parent_meta == null)
                throw new Error('Impossible de créer cet enregistrement sans parent');

            meta.parent = {
                form: parent_meta.form.key,
                ulid: parent_meta.ulid,
                version: parent_meta.version
            };
        }

        return meta;
    }

    async function loadRecord(ulid, version) {
        let key = `${profile.userid}:${ulid}`;
        let obj = await db.load('rec_records', key);

        if (obj == null)
            throw new Error('L\'enregistrement demandé n\'existe pas');

        let record = await goupile.decryptLocal(obj.enc);
        let fragments = record.fragments;

        // Deleted record
        if (fragments[fragments.length - 1].type === 'delete')
            throw new Error('L\'enregistrement demandé est supprimé');

        if (version == null) {
            version = fragments.length;
        } else if (version > fragments.length) {
            throw new Error(`Cet enregistrement n'a pas de version ${version}`);
        }

        let values = {};
        let children = new Map;
        let status = new Set;
        for (let i = 0; i < version; i++) {
            let fragment = fragments[i];

            switch (fragment.type) {
                case 'save': {
                    Object.assign(values, fragment.values);
                    status.add(fragment.page);
                } break;
                case 'save_child': {
                    children.set(fragment.child.form, fragment.child);
                    status.add(fragment.child.form);
                } break;
                case 'delete_child': {
                    children.delete(fragment.child.form);
                    status.delete(fragment.child.form);
                } break;
            }
        }
        for (let fragment of fragments) {
            fragment.mtime = new Date(fragment.mtime);

            delete fragment.child;
            delete fragment.values;
        }

        let meta = {
            form: app.forms.get(record.form),
            ulid: ulid,
            hid: record.hid,
            version: version,
            parent: record.parent,
            children: children,
            mtime: fragments[version - 1].mtime,
            fragments: fragments,
            status: status,
            values: values
        };
        if (meta.form == null)
            throw new Error(`Le formulaire '${record.form}' n'existe pas ou plus`);

        return meta;
    }

    // Assumes from != to
    function computePath(from, to) {
        let prefix_len = 0;
        while (prefix_len < from.parents.length && prefix_len < to.parents.length) {
            let parent1 = from.parents[from.parents.length - prefix_len - 1];
            let parent2 = to.parents[to.parents.length - prefix_len - 1];

            if (parent1 !== parent2)
                break;

            prefix_len++;
        }

        let up = from.parents.length - prefix_len - 1;
        let down = to.parents.length - prefix_len - 1;

        if (from.parents[up] === to) {
            let path = {
                up: [...from.parents.slice(0, up), to],
                down: []
            };
            return path;
        } else if (to.parents[down] === from) {
            let path = {
                up: [],
                down: [...to.parents.slice(0, down).reverse(), to]
            };
            return path;
        } else if (prefix_len) {
            let path = {
                up: from.parents.slice(0, up + 2),
                down: [...to.parents.slice(0, down - 1).reverse(), to]
            };
            return path;
        } else {
            return null;
        }
    }

    function isFormEnabled(form, meta0) {
        for (let page of form.pages.values()) {
            if (isPageEnabled(page, meta0))
                return true;
        }

        return false;
    }

    function isPageEnabled(page, meta0) {
        if (typeof page.enabled === 'function') {
            return page.enabled(meta0);
        } else {
            return page.enabled;
        }
    }

    this.run = async function(push_history = false) {
        // Is the user developing?
        {
            let range = IDBKeyRange.bound(profile.userid + ':', profile.userid + '`', false, true);
            let count = await db.count('fs_files', range);

            develop = !!count;
        }

        // Update browser URL
        {
            let url = route.page.url;
            if (route.version > 0) {
                url += `/${route.ulid}`;
                if (route.version < form_meta.fragments.length)
                    url += `@${route.version}`;
            } else if (form_meta.parent != null) {
                url += `/${form_meta.parent.ulid}`;
            }
            goupile.syncHistory(url, push_history);
        }

        // Sync editor (if needed)
        if (ui.isPanelEnabled('editor')) {
            if (editor_filename == null || editor_filename.startsWith('pages/'))
                editor_filename = route.page.filename;

            await syncEditor();
        }

        // Load rows for data panel
        if (ui.isPanelEnabled('data')) {
            if (data_form !== form_chain[0].form) {
                data_form = form_chain[0].form;
                data_rows = null;
            }

            if (data_rows == null) {
                let objects;
                if (!data_form.parents.length) {
                    let range = IDBKeyRange.only(`${profile.userid}/${data_form.key}`);
                    objects = await db.loadAll('rec_records/form', range);
                } else {
                    let range = IDBKeyRange.only(`${profile.userid}:${form_meta.parent.ulid}`);
                    objects = await db.loadAll('rec_records/parent', range);
                }

                data_rows = [];
                for (let obj of objects) {
                    try {
                        let record = await goupile.decryptLocal(obj.enc);
                        let fragments = record.fragments;

                        if (fragments[fragments.length - 1].type == 'delete')
                            continue;
                        if (fragments.length === 1 && fragments[0].type === 'fake')
                            continue;

                        let children = new Map;
                        let status = new Set;
                        for (let fragment of fragments) {
                            switch (fragment.type) {
                                case 'save': { status.add(fragment.page); } break;
                                case 'save_child': {
                                    children.set(fragment.child.form, fragment.child);
                                    status.add(fragment.child.form);
                                } break;
                                case 'delete_child': {
                                    children.delete(fragment.child.form);
                                    status.delete(fragment.child.form);
                                } break;
                            }
                        }

                        let row = {
                            ulid: record.ulid,
                            hid: record.hid,
                            children: children,
                            status: status
                        };
                        data_rows.push(row);
                    } catch (err) {
                        console.log(err);
                    }
                }
            }
        }

        // Fetch page code (for page panel)
        page_code = await fetchCode(route.page.filename);

        ui.render();
    };
    this.run = util.serializeAsync(this.run);

    async function fetchCode(filename) {
        // Anything in the editor?
        {
            let buffer = editor_buffers.get(filename);
            if (buffer != null && !buffer.reload)
                return buffer.session.doc.getValue();
        }

        // Try the cache
        {
            let code = code_cache.get(filename);
            if (code != null)
                return code;
        }

        // Try locally saved files
        if (goupile.isAuthorized()) {
            let key = `${profile.userid}:${filename}`;
            let file = await db.load('fs_files', key);

            if (file != null) {
                let code = await file.blob.text();
                code_cache.set(filename, code);
                return code;
            }
        }

        // The server is our last hope
        {
            let response = await net.fetch(`${ENV.base_url}files/${filename}`);

            if (response.ok) {
                let code = await response.text();
                code_cache.set(filename, code);
                return code;
            } else {
                return '';
            }
        }
    }

    async function backupClientData(dest) {
        let progress = log.progress('Archivage sécurisé des données');

        try {
            let range = IDBKeyRange.bound(profile.userid + ':', profile.userid + '`', false, true);
            let objects = await db.loadAll('rec_records', range);

            let records = [];
            for (let obj of objects) {
                try {
                    let record = await goupile.decryptLocal(obj.enc);
                    records.push(record);
                } catch (err) {
                    console.log(err);
                }
            }

            let enc = await goupile.encryptBackup(records);
            let json = JSON.stringify(enc);

            if (dest === 'server') {
                let response = await fetch(`${ENV.base_url}api/files/backup`, {
                    method: 'POST',
                    body: JSON.stringify(enc)
                });

                if (response.ok) {
                    console.log('Archive sécurisée envoyée');
                    progress.close();
                } else if (response.status === 409) {
                    console.log('Archivage ignoré (archive récente)');
                    progress.close();
                } else {
                    let err = (await response.text()).trim();
                    throw new Error(err);
                }
            } else if (dest === 'file') {
                let blob = new Blob([json], {
                    type: 'application/octet-stream'
                });

                console.log('Archive sécurisée créée');
                progress.close();

                let filename = `${ENV.base_url.replace(/\//g, '')}_${profile.username}_${dates.today()}.backup`;
                util.saveBlob(blob, filename);
            } else {
                throw new Error(`Invalid backup destination '${dest}'`);
            }
        } catch (err) {
            progress.close();
            throw err;
        }
    }
};
