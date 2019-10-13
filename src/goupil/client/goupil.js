// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// These globals are initialized below
let g_assets = null;
let g_records = null;

let goupil = (function() {
    let self = this;

    let event_src;

    let runner = dev;

    document.addEventListener('readystatechange', e => {
        if (document.readyState === 'complete')
            initGoupil();
    });

    async function initGoupil() {
        log.pushHandler(log.notifyHandler);
        initNavigation();

        let db = await openDatabase();
        g_assets = new AssetManager(db);
        g_records = new RecordManager(db);

        if (typeof app !== 'undefined') {
            await app.init();
            runner = app;
        }
        if (typeof dev !== 'undefined') {
            await dev.init();
            runner = dev;
        }

        self.go(window.location.href, false);
    }

    function initNavigation() {
        window.addEventListener('popstate', e => self.go(window.location.href, false));

        util.interceptLocalAnchors((e, href) => {
            self.go(href);
            e.preventDefault();
        });
    }

    async function openDatabase() {
        let storage_warning = 'Local data may be cleared by the browser under storage pressure, ' +
                              'check your privacy settings for this website';

        if (navigator.storage && navigator.storage.persist) {
            navigator.storage.persist().then(granted => {
                // NOTE: For some reason this does not seem to work correctly on my Firefox profile,
                // where granted is always true. Might come from an extension or some obscure privacy
                // setting. Investigate.
                if (!granted)
                    log.error(storage_warning);
            });
        } else {
            log.error(storage_warning);
        }

        let db_name = `goupil_${env.project_key}`;
        let db = await idb.open(db_name, 8, (db, old_version) => {
            switch (old_version) {
                case null: {
                    db.createObjectStore('pages', {keyPath: 'key'});
                    db.createObjectStore('settings');
                } // fallthrough
                case 1: {
                    db.createObjectStore('data', {keyPath: 'id'});
                } // fallthrough
                case 2: {
                    db.createObjectStore('variables', {keyPath: 'key'});
                } // fallthrough
                case 3: {
                    db.deleteObjectStore('data');
                    db.deleteObjectStore('settings');

                    db.createObjectStore('assets', {keyPath: 'key'});
                    db.createObjectStore('records', {keyPath: 'id'});
                } // fallthrough
                case 4: {
                    db.deleteObjectStore('records');
                    db.deleteObjectStore('variables');
                    db.createObjectStore('records', {keyPath: 'tkey'});
                    db.createObjectStore('variables', {keyPath: 'tkey'});
                } // fallthrough
                case 5: {
                    db.deleteObjectStore('pages');
                } // fallthrough
                case 6: {
                    db.deleteObjectStore('records');
                    db.deleteObjectStore('variables');
                    db.createObjectStore('form_records', {keyPath: 'tkey'});
                    db.createObjectStore('form_variables', {keyPath: 'tkey'});
                } // fallthrough
                case 7: {
                    db.deleteObjectStore('assets');
                    db.createObjectStore('assets');
                } // fallthrough
            }
        });

        return db;
    }

    this.go = function(url, history = true) {
        runner.go();
    };

    this.listenToServerEvent = function(event, func) {
        if (!event_src) {
            event_src = new EventSource(`${env.base_url}api/events`);
            event_src.onerror = e => event_src = null;
        }

        event_src.addEventListener(event, func);
    };

    this.makePagePath = function(key) { return `pages/${key}.js`; };
    this.makeBlobPath = function(key) { return `static/${key}`; };

    return this;
}).call({});
