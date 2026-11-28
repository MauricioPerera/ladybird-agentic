(() => {
  'use strict';

  const notesListEl = document.getElementById('notes-list');
  const filenameEl = document.getElementById('note-filename');
  const contentEl = document.getElementById('note-content');
  const editorStatusEl = document.getElementById('editor-status');
  const runtimeStatusEl = document.getElementById('runtime-status');
  const runtimeMetaEl = document.getElementById('runtime-meta');
  const webmcpStatusEl = document.getElementById('webmcp-status');
  const accountPillEl = document.getElementById('account-pill');
  const accountSelectEl = document.getElementById('account-select');
  const storageMetaEl = document.getElementById('storage-meta');

  let currentNote = '';
  let activeAccount = null;
  let webmcpRegistered = false;

  function normalizeFilename(rawName) {
    const clean = String(rawName || '').trim().replace(/^notes\//, '');
    if (!clean || clean.includes('..') || clean.includes('/') || clean.includes('\\')) {
      throw new Error('Provide a simple note filename like welcome.md');
    }
    return clean.endsWith('.md') || clean.endsWith('.txt') ? clean : `${clean}.md`;
  }

  function setStatus(message) {
    editorStatusEl.textContent = message;
  }

  async function refreshAccounts() {
    const data = await window.ladybirdApp.accounts.list();
    const accounts = data.accounts || [];
    accountSelectEl.innerHTML = '';
    for (const acc of accounts) {
      const opt = document.createElement('option');
      opt.value = acc.id;
      opt.textContent = acc.active ? `${acc.label} (${acc.id}) [active]` : `${acc.label} (${acc.id})`;
      if (acc.active) opt.selected = true;
      accountSelectEl.appendChild(opt);
    }
    return data;
  }

  async function refreshNotes() {
    const res = await window.ladybirdApp.fs.listDir('notes');
    const files = (res.entries || [])
      .filter((entry) => !entry.isDirectory)
      .sort((a, b) => a.name.localeCompare(b.name));

    notesListEl.innerHTML = '';
    for (const file of files) {
      const li = document.createElement('li');
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'note-item' + (file.name === currentNote ? ' is-active' : '');
      btn.textContent = file.name;
      btn.addEventListener('click', () => openNote(file.name));
      li.appendChild(btn);
      notesListEl.appendChild(li);
    }
    return files;
  }

  async function openNote(name) {
    const clean = normalizeFilename(name);
    const data = await window.ladybirdApp.fs.readText(`notes/${clean}`);
    currentNote = clean;
    filenameEl.value = clean;
    contentEl.value = data.content || '';
    if (window.ladybirdApp.kv) {
      await window.ladybirdApp.kv.set('ui:lastNote', clean).catch(() => {});
    }
    setStatus(`Loaded notes/${clean}`);
    await refreshNotes();
  }

  async function saveNote(name, content) {
    const clean = normalizeFilename(name);
    const res = await window.ladybirdApp.fs.writeText(`notes/${clean}`, String(content ?? ''));
    currentNote = clean;
    filenameEl.value = clean;
    contentEl.value = String(content ?? '');
    if (window.ladybirdApp.kv) {
      await window.ladybirdApp.kv.set('ui:lastNote', clean).catch(() => {});
    }
    setStatus(`Saved notes/${clean} (${res.bytesWritten} bytes)`);
    await refreshNotes();
    return { path: `notes/${clean}`, bytesWritten: res.bytesWritten };
  }

  async function deleteNote(name) {
    const clean = normalizeFilename(name);
    await window.ladybirdApp.fs.deleteFile(`notes/${clean}`);
    if (currentNote === clean) {
      currentNote = '';
      filenameEl.value = '';
      contentEl.value = '';
    }
    setStatus(`Deleted notes/${clean}`);
    await refreshNotes();
    return { deleted: `notes/${clean}` };
  }

  function registerWebMCPTools() {
    if (webmcpRegistered) {
      return;
    }
    const ctx = document.modelContext;
    if (!ctx || typeof ctx.registerTool !== 'function') {
      webmcpStatusEl.textContent = 'WebMCP: unavailable';
      return;
    }
    webmcpRegistered = true;

    const tools = [
      {
        name: 'get_account_info',
        description: 'Return current account, profile name, origin, and registered accounts for this app.',
        inputSchema: { type: 'object', properties: {}, additionalProperties: false },
        execute: async () => {
          const [current, listRes] = await Promise.all([
            window.ladybirdApp.accounts.current(),
            window.ladybirdApp.accounts.list()
          ]);
          return { origin: window.location.origin, current, accounts: listRes.accounts || [] };
        }
      },
      {
        name: 'kv_get',
        description: 'Read a JSON value by key from persistent SQLite KV storage for the active account.',
        inputSchema: {
          type: 'object',
          properties: { key: { type: 'string', description: 'Storage key' } },
          required: ['key'],
          additionalProperties: false
        },
        execute: async ({ key }) => window.ladybirdApp.kv.get(key)
      },
      {
        name: 'kv_set',
        description: 'Persist a JSON value by key in SQLite KV storage for the active account.',
        inputSchema: {
          type: 'object',
          properties: {
            key: { type: 'string', description: 'Storage key' },
            value: { description: 'Any JSON value' }
          },
          required: ['key', 'value'],
          additionalProperties: false
        },
        execute: async ({ key, value }) => window.ladybirdApp.kv.set(key, value)
      },
      {
        name: 'kv_list',
        description: 'List keys in persistent SQLite KV storage for the active account.',
        inputSchema: {
          type: 'object',
          properties: { prefix: { type: 'string', description: 'Optional key prefix' } },
          additionalProperties: false
        },
        execute: async ({ prefix }) => window.ladybirdApp.kv.list(prefix)
      },
      {
        name: 'list_notes',
        description: 'List markdown/text note files stored in the Agentic Notes workspace.',
        inputSchema: { type: 'object', properties: {}, additionalProperties: false },
        execute: async () => ({ notes: (await refreshNotes()).map((f) => f.name) })
      },
      {
        name: 'read_note',
        description: 'Read a note file by filename from the Agentic Notes workspace.',
        inputSchema: {
          type: 'object',
          properties: { name: { type: 'string', description: 'Filename (e.g. welcome.md)' } },
          required: ['name'],
          additionalProperties: false
        },
        execute: async ({ name }) => {
          const clean = normalizeFilename(name);
          const data = await window.ladybirdApp.fs.readText(`notes/${clean}`);
          return { name: clean, content: data.content };
        }
      },
      {
        name: 'create_note',
        description: 'Create or update a note file in the Agentic Notes workspace.',
        inputSchema: {
          type: 'object',
          properties: {
            name: { type: 'string', description: 'Filename (e.g. meeting-notes.md)' },
            content: { type: 'string', description: 'Markdown note content' }
          },
          required: ['name', 'content'],
          additionalProperties: false
        },
        execute: async ({ name, content }) => saveNote(name, content)
      },
      {
        name: 'delete_note',
        description: 'Delete a note file from the Agentic Notes workspace.',
        inputSchema: {
          type: 'object',
          properties: { name: { type: 'string', description: 'Filename to delete' } },
          required: ['name'],
          additionalProperties: false
        },
        annotations: { destructiveHint: true },
        execute: async ({ name }) => deleteNote(name)
      }
    ];

    for (const tool of tools) {
      ctx.registerTool(tool);
    }
    webmcpStatusEl.textContent = `WebMCP: ${tools.length} tools registered`;
  }

  document.getElementById('new-note-btn').addEventListener('click', () => {
    currentNote = '';
    filenameEl.value = `note-${Date.now()}.md`;
    contentEl.value = '# New Note\n\n';
    setStatus('Draft ready (not saved yet)');
  });

  document.getElementById('refresh-btn').addEventListener('click', () => {
    refreshNotes().catch((err) => setStatus(err.message));
  });

  document.getElementById('save-note-btn').addEventListener('click', () => {
    saveNote(filenameEl.value, contentEl.value).catch((err) => setStatus(err.message));
  });

  document.getElementById('delete-note-btn').addEventListener('click', () => {
    deleteNote(filenameEl.value).catch((err) => setStatus(err.message));
  });

  document.getElementById('open-account-btn').addEventListener('click', async () => {
    const selectedId = accountSelectEl.value;
    if (!selectedId) return;
    if (activeAccount && selectedId === activeAccount.id) {
      setStatus(`Account '${selectedId}' is already open in this window`);
      return;
    }
    try {
      await window.ladybirdApp.accounts.launch(selectedId, { createIfMissing: false });
      setStatus(`Launched separate window for account '${selectedId}'`);
    } catch (err) {
      setStatus(err.message);
    }
  });

  document.getElementById('new-account-btn').addEventListener('click', async () => {
    const rawId = window.prompt('New account slug (lowercase letters, digits, -, _):', 'work');
    if (!rawId) return;
    const cleanId = rawId.trim().toLowerCase();
    const label = window.prompt('Display label for this account:', cleanId) || cleanId;
    try {
      await window.ladybirdApp.accounts.create(cleanId, label.trim());
      await refreshAccounts();
      await window.ladybirdApp.accounts.launch(cleanId, { createIfMissing: true });
      setStatus(`Created & launched account '${cleanId}'`);
    } catch (err) {
      setStatus(err.message);
    }
  });

  async function boot() {
    if (!window.ladybirdApp) {
      runtimeStatusEl.textContent = 'Bridge missing';
      return;
    }

    const sys = await window.ladybirdApp.system.info();
    activeAccount = await window.ladybirdApp.accounts.current();
    accountPillEl.textContent = `${activeAccount.label} (${activeAccount.id})`;
    await refreshAccounts();

    // Update persistent counters in both SQLite KV and origin-scoped localStorage
    const lsVisits = (parseInt(window.localStorage.getItem('lb_visits') || '0', 10) || 0) + 1;
    window.localStorage.setItem('lb_visits', String(lsVisits));

    let kvVisits = 1;
    let lastNote = null;
    if (window.ladybirdApp.kv) {
      const prevKv = await window.ladybirdApp.kv.get('meta:visits');
      kvVisits = (prevKv && prevKv.found && typeof prevKv.value === 'number' ? prevKv.value : 0) + 1;
      await window.ladybirdApp.kv.set('meta:visits', kvVisits);
      const lastEntry = await window.ladybirdApp.kv.get('ui:lastNote');
      if (lastEntry && lastEntry.found && typeof lastEntry.value === 'string') {
        lastNote = lastEntry.value;
      }
    }

    storageMetaEl.textContent = `Origin :${sys.port}${sys.stablePort ? ' (stable)' : ''} · KV visits: ${kvVisits} · localStorage visits: ${lsVisits}`;
    runtimeStatusEl.textContent = `Connected (${sys.platform})`;
    runtimeMetaEl.textContent = `v${sys.version} · Profile ${activeAccount.profileName}${sys.mcpPort ? ` · MCP :${sys.mcpPort}` : ''}`;

    let files = await refreshNotes();
    if (files.length === 0) {
      await saveNote(
        'welcome.md',
        `# Welcome (${activeAccount.label})\n\nAccount: \`${activeAccount.id}\` (profile \`${activeAccount.profileName}\`)\n\n- Local filesystem storage jailed to \`notes/\`\n- Persistent SQLite KV storage in \`AppStore.db\`\n- Stable origin \`${window.location.origin}\` keeps \`localStorage\` across restarts\n`
      );
      files = await refreshNotes();
    }
    const targetNote = lastNote && files.some((f) => f.name === lastNote) ? lastNote : (files[0] && files[0].name);
    if (targetNote) {
      await openNote(targetNote);
    }

    window.addEventListener('ladybird:webmcp-ready', registerWebMCPTools);
    registerWebMCPTools();
  }

  boot().catch((err) => {
    runtimeStatusEl.textContent = 'Boot error';
    setStatus(err.message);
  });
})();
