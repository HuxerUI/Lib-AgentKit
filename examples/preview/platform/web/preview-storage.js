function openDatabase(name) {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open(name, 2);
    open.onupgradeneeded = ({ oldVersion }) => {
      const db = open.result;
      if (oldVersion === 0) {
        db.createObjectStore('meta');
        db.createObjectStore('records', { keyPath: ['collection', 'key'] });
        db.createObjectStore('commits');
        db.createObjectStore('navigation');
        db.createObjectStore('preferences', { keyPath: ['principal', 'session'] }).createIndex('principal', 'principal');
      }
      db.createObjectStore('workspaces', { keyPath: 'id' });
    };
    let blocked = false;
    open.onblocked = () => {
      blocked = true;
      reject(new Error('Another tab blocks opening Preview storage'));
    };
    open.onerror = () => reject(open.error);
    open.onsuccess = () => {
      if (blocked) open.result.close();
      else resolve(open.result);
    };
  });
}

function transact(db, names, write, operation) {
  return new Promise((resolve, reject) => {
    const tx = write ? db.transaction(names, 'readwrite', { durability: 'strict' }) : db.transaction(names);
    let value = null;
    let error;
    tx.oncomplete = () => resolve(value);
    tx.onabort = () => reject(error ?? tx.error ?? new Error('Preview storage transaction aborted'));
    const guard = (action) => {
      try {
        action();
      } catch (failure) {
        error = failure;
        tx.abort();
      }
    };
    const collect = (requests, consume) => {
      const results = [];
      let remaining = requests.length;
      for (const [index, request] of requests.entries()) {
        request.onsuccess = () => guard(() => {
          results[index] = request.result;
          if (--remaining === 0) consume(results);
        });
      }
    };
    guard(() => operation((name) => tx.objectStore(name), collect, (result) => { value = result; }));
  });
}

function commit(db, request) {
  return transact(db, ['meta', 'records', 'commits'], true, (store, read, finish) => {
    const meta = store('meta');
    const records = store('records');
    const commits = store('commits');
    const keys = request.writes.map(({ collection, key }) => [collection, key]);
    read([meta.get('revision'), commits.get(request.id), ...keys.map((key) => records.get(key))],
      ([storedRevision, previous, ...currentRecords]) => {
        const current = storedRevision ?? '0';
        const payload = JSON.stringify(request);
        const conflict = (message, revision) => finish({ conflict: message, revision });
        if (previous) {
          if (previous.payload === payload) finish({ revision: previous.revision });
          else conflict('Commit ID already has a different payload', current);
          return;
        }
        if (current !== request.expected) {
          conflict('Store revision changed', current);
          return;
        }
        if (BigInt(current) >= 9223372036854775807n) throw new Error('Store revision exhausted');
        for (const [index, write] of request.writes.entries()) {
          const found = currentRecords[index]?.revision ?? null;
          if (found !== write.expected) {
            conflict('Store record revision changed', found);
            return;
          }
        }
        const revision = (BigInt(current) + 1n).toString();
        for (const [index, { collection, key, data }] of request.writes.entries()) {
          if (data === null) records.delete(keys[index]);
          else records.put({ collection, key, data, revision });
        }
        commits.add({ revision, payload }, request.id);
        meta.put(revision, 'revision');
        finish({ revision });
      });
  });
}

function checkedWorkspacePath(path) {
  const parts = path.split('/');
  if (parts.length > 32 || parts.some((part) => !part || part === '.' || part === '..' ||
      part.toLowerCase() === '.git' || /[\\:\x00-\x1f]/u.test(part))) {
    throw new DOMException('Choose a path inside this workspace', 'DataError');
  }
  return parts;
}

function visibleWorkspace(entry) {
  return { id: entry.id, name: entry.handle.name };
}

function workspaceRegistry(db) {
  return transact(db, ['meta', 'workspaces'], false, (store, read, finish) => {
    read([store('meta').get('workspaceRevision'), store('workspaces').getAll()],
      ([revision, entries]) => finish({ revision: revision ?? 0, entries }));
  });
}

async function rememberWorkspace(db, id, handle) {
  for (let attempt = 0; attempt < 8; ++attempt) {
    const { revision, entries } = await workspaceRegistry(db);
    for (const entry of entries) {
      if (await handle.isSameEntry(entry.handle)) return entry;
    }
    const entry = { id, handle };
    const saved = await transact(db, ['meta', 'workspaces'], true, (store, read, finish) => {
      const meta = store('meta');
      read([meta.get('workspaceRevision')], ([current]) => {
        const matches = (current ?? 0) === revision;
        if (matches) {
          store('workspaces').add(entry);
          meta.put(revision + 1, 'workspaceRevision');
        }
        finish(matches);
      });
    });
    if (saved) return entry;
  }
  throw new Error('The workspace registry changed in another tab. Open the folder again.');
}

async function workspaceOperation(db, handles, operation, request) {
  try {
    if (operation === 'loadWorkspaces') {
      const { entries } = await workspaceRegistry(db);
      for (const entry of entries) handles.set(entry.id, entry.handle);
      return entries.map(visibleWorkspace);
    }
    if (operation === 'pickWorkspace') {
      if (!globalThis.isSecureContext || typeof globalThis.showDirectoryPicker !== 'function')
        return { error: 'unsupported', message: 'Folder access requires a browser with a directory picker.' };
      let handle;
      try {
        handle = await globalThis.showDirectoryPicker({ mode: 'read' });
      } catch (error) {
        if (error?.name === 'AbortError') return null;
        throw error;
      }
      const entry = await rememberWorkspace(await db, request.id, handle);
      handles.set(entry.id, handle);
      return visibleWorkspace(entry);
    }
    const handle = handles.get(request.id);
    if (!handle || handle.kind !== 'directory')
      return { error: 'notFound', message: 'The conversation folder is unavailable.' };
    const permission = { mode: 'read' };
    if (operation === 'authorizeWorkspace') {
      if (await handle.requestPermission(permission) !== 'granted')
        return { error: 'permission', message: 'Allow access to the conversation folder to continue.' };
      return null;
    }
    if (await handle.queryPermission(permission) !== 'granted')
      return { error: 'permission', message: 'Folder access expired. Reconnect the folder to continue.' };
    if (operation === 'listWorkspaceReports') {
      const parts = request.path ? checkedWorkspacePath(request.path) : [];
      let folder = handle;
      for (const part of parts) folder = await folder.getDirectoryHandle(part);
      const entries = [];
      for await (const entry of folder.values()) {
        if (entry.name.toLowerCase() === '.git') continue;
        if (entry.kind === 'directory' || entry.name.toLowerCase().endsWith('.json')) {
          entries.push({ name: entry.name, directory: entry.kind === 'directory' });
        }
        if (entries.length > 256)
          return { error: 'budget', message: 'This folder has too many entries to browse. Import the report directly.' };
      }
      return entries.sort((a, b) => Number(b.directory) - Number(a.directory) || a.name.localeCompare(b.name));
    }
    if (operation === 'readWorkspaceReport') {
      const parts = checkedWorkspacePath(request.path);
      const name = parts.pop();
      if (!name.toLowerCase().endsWith('.json'))
        throw new DOMException('Choose a JSON report', 'DataError');
      let folder = handle;
      for (const part of parts) folder = await folder.getDirectoryHandle(part);
      const file = await (await folder.getFileHandle(name)).getFile();
      if (file.size > 64 * 1024)
        return { error: 'budget', message: 'Choose a JSON report up to 64 KiB.' };
      const text = new TextDecoder('utf-8', { fatal: true }).decode(await file.arrayBuffer());
      if (await handle.queryPermission(permission) !== 'granted')
        return { error: 'permission', message: 'Folder access changed while reading the report.' };
      return { name, text };
    }
    throw new Error('Unknown workspace operation');
  } catch (error) {
    switch (error?.name) {
      case 'NotAllowedError':
      case 'SecurityError':
        return { error: 'permission', message: 'Allow access to the conversation folder to continue.' };
      case 'NotFoundError':
      case 'TypeMismatchError':
        return { error: 'notFound', message: 'The selected folder or report is unavailable.' };
      case 'DataError':
      case 'TypeError':
        return { error: 'invalid', message: 'Choose a JSON report inside the conversation folder.' };
      case 'NotReadableError':
        return { error: 'unreadable', message: 'The selected report could not be read.' };
      default: throw error;
    }
  }
}

function createPreviewStorage() {
  const connections = new Map();
  const workspaces = new Map();
  return async (name, operation, request) => {
    if (!connections.has(name)) {
      const opening = openDatabase(name).then((db) => {
        db.onversionchange = () => {
          db.close();
          connections.delete(name);
        };
        db.onclose = () => connections.delete(name);
        return db;
      }).catch((error) => {
        connections.delete(name);
        throw error;
      });
      connections.set(name, opening);
    }
    if (operation === 'pickWorkspace' || operation === 'authorizeWorkspace') {
      if (!workspaces.has(name)) workspaces.set(name, new Map());
      return workspaceOperation(connections.get(name), workspaces.get(name), operation, request);
    }
    const db = await connections.get(name);
    switch (operation) {
      case 'open': return null;
      case 'loadWorkspaces':
      case 'listWorkspaceReports':
      case 'readWorkspaceReport':
        if (!workspaces.has(name)) workspaces.set(name, new Map());
        return workspaceOperation(db, workspaces.get(name), operation, request);
      case 'load':
        return transact(db, ['meta', 'records'], false, (store, read, finish) => {
          read([store('meta').get('revision'), store('records').getAll()],
            ([revision, records]) => finish({ revision: revision ?? '0', records }));
        });
      case 'commit': return commit(db, request);
      case 'findCommit':
        return transact(db, 'commits', false, (store, read, finish) => {
          read([store('commits').get(request.id)], ([found]) => finish(found?.revision ?? null));
        });
      case 'loadPreferences':
        return transact(db, 'preferences', false, (store, read, finish) => {
          read([store('preferences').index('principal').getAll(request.principal)], ([entries]) => finish(entries));
        });
      case 'savePreference':
        return transact(db, 'preferences', true, (store) => { store('preferences').put(request); });
      case 'loadNavigation':
        return transact(db, 'navigation', false, (store, read, finish) => {
          read([store('navigation').get(request.principal)], ([found]) => finish(found ?? null));
        });
      case 'saveNavigation':
        return transact(db, 'navigation', true, (store, read, finish) => {
          const navigation = store('navigation');
          read([navigation.get(request.principal)], ([found]) => {
            const matches = (found ?? null) === request.expected;
            if (matches) navigation.put(request.value, request.principal);
            finish(matches);
          });
        });
      default: throw new Error('Unknown Preview storage operation');
    }
  };
}

Module['agentkitStorage'] = createPreviewStorage();
