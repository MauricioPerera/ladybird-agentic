import { Bash, InMemoryFs, defineCommand } from '/vendor/just-bash.mjs';

const WORKSPACE_ROOT = '/workspace';

const terminalLogEl = document.getElementById('terminal-log');
const cmdFormEl = document.getElementById('cmd-form');
const cmdInputEl = document.getElementById('cmd-input');
const fileListEl = document.getElementById('file-list');
const accountPillEl = document.getElementById('account-pill');
const accountSelectEl = document.getElementById('account-select');
const storageMetaEl = document.getElementById('storage-meta');
const bootStatusEl = document.getElementById('boot-status');
const runtimeMetaEl = document.getElementById('runtime-meta');
const syncStatusEl = document.getElementById('sync-status');
const webmcpStatusEl = document.getElementById('webmcp-status');

let bash = null;
let memFs = null;
let activeAccount = null;
let syncedSnapshot = new Map();
let commandHistory = [];
let historyIndex = -1;
let execQueue = Promise.resolve();
let webmcpRegistered = false;

function appendTerminal(cmd, stdout, stderr, exitCode) {
  const wrap = document.createElement('div');
  wrap.className = 'term-entry';

  if (cmd !== null) {
    const cmdLine = document.createElement('div');
    cmdLine.className = 'term-cmd';
    cmdLine.textContent = `$ ${cmd}`;
    wrap.appendChild(cmdLine);
  }
  if (stdout) {
    const outEl = document.createElement('div');
    outEl.className = 'term-out';
    outEl.textContent = stdout.replace(/\n$/, '');
    wrap.appendChild(outEl);
  }
  if (stderr) {
    const errEl = document.createElement('div');
    errEl.className = 'term-err';
    errEl.textContent = stderr.replace(/\n$/, '');
    wrap.appendChild(errEl);
  }
  if (typeof exitCode === 'number' && exitCode !== 0) {
    const exitEl = document.createElement('div');
    exitEl.className = 'term-exit';
    exitEl.textContent = `[exit ${exitCode}]`;
    wrap.appendChild(exitEl);
  }

  terminalLogEl.appendChild(wrap);
  terminalLogEl.scrollTop = terminalLogEl.scrollHeight;
}

async function hydrateDirFromHost(diskRelDir, memDir) {
  await memFs.mkdir(memDir, { recursive: true });
  let listing;
  try {
    listing = await window.ladybirdApp.fs.listDir(diskRelDir);
  } catch (e) {
    return;
  }
  for (const entry of listing.entries || []) {
    const nextDisk = `${diskRelDir}/${entry.name}`;
    const nextMem = `${memDir}/${entry.name}`;
    if (entry.isDirectory) {
      await hydrateDirFromHost(nextDisk, nextMem);
    } else {
      try {
        const res = await window.ladybirdApp.fs.readText(nextDisk);
        const text = String(res.content ?? '');
        await memFs.writeFile(nextMem, text, 'utf8');
        syncedSnapshot.set(nextDisk, text);
      } catch (e) {
        // Skip binary/unreadable files cleanly
      }
    }
  }
}

async function seedStarterWorkspace() {
  const seeds = {
    'workspace/welcome.md': [
      '# Bash Workbench (PoC)',
      '',
      'Sandbox UNIX shell running `vercel-labs/just-bash` inside Ladybird App Mode.',
      '',
      `- Account: ${activeAccount.label} (${activeAccount.id})`,
      `- Profile: ${activeAccount.profileName}`,
      '- `/workspace` syncs with `window.ladybirdApp.fs` (jailed per account)',
      '- Built-in commands: `kv`, `accounts`, `sysinfo`, `jq`, `awk`, `sed`, `grep`, `rg`',
      '',
    ].join('\n'),
    'workspace/tasks.json': JSON.stringify(
      [
        { id: 1, title: 'Verify stable loopback port', owner: 'ops', done: true },
        { id: 2, title: 'Run jq query inside LibJS', owner: 'agent', done: false },
        { id: 3, title: 'Persist SQLite KV state across restart', owner: 'db', done: false },
      ],
      null,
      2
    ) + '\n',
    'workspace/sales.csv': [
      'id,client,amount,status',
      '101,Acme,1250,paid',
      '102,Northwind,840,paid',
      '103,Globex,2190,pending',
    ].join('\n') + '\n',
  };

  for (const [diskPath, content] of Object.entries(seeds)) {
    await window.ladybirdApp.fs.writeText(diskPath, content);
    await memFs.writeFile(`/${diskPath}`, content, 'utf8');
    syncedSnapshot.set(diskPath, content);
  }
}

async function walkMemWorkspace(memDir, diskPrefix, outMap) {
  let names = [];
  try {
    names = await memFs.readdir(memDir);
  } catch (e) {
    return;
  }
  for (const name of names) {
    const memPath = `${memDir}/${name}`;
    const diskPath = `${diskPrefix}/${name}`;
    try {
      const st = await memFs.stat(memPath);
      if (st.isDirectory) {
        await walkMemWorkspace(memPath, diskPath, outMap);
      } else if (st.isFile) {
        const text = await memFs.readFile(memPath, 'utf8');
        outMap.set(diskPath, String(text ?? ''));
      }
    } catch (e) {}
  }
}

async function syncWorkspaceToDisk() {
  const current = new Map();
  await walkMemWorkspace(WORKSPACE_ROOT, 'workspace', current);

  let written = 0;
  let deleted = 0;

  for (const [diskPath, content] of current.entries()) {
    if (syncedSnapshot.get(diskPath) !== content) {
      await window.ladybirdApp.fs.writeText(diskPath, content);
      syncedSnapshot.set(diskPath, content);
      written++;
    }
  }

  for (const oldPath of Array.from(syncedSnapshot.keys())) {
    if (!current.has(oldPath)) {
      await window.ladybirdApp.fs.deleteFile(oldPath).catch(() => {});
      syncedSnapshot.delete(oldPath);
      deleted++;
    }
  }

  syncStatusEl.textContent = `Workspace: ${current.size} files (last sync: +${written}/-${deleted})`;
  renderFileList(Array.from(current.keys()).sort());
  return { fileCount: current.size, written, deleted };
}

function renderFileList(diskPaths) {
  fileListEl.innerHTML = '';
  for (const p of diskPaths) {
    const memPath = `/${p}`;
    const li = document.createElement('li');
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'file-btn';
    btn.textContent = memPath;
    btn.addEventListener('click', () => {
      runBashCommand(`cat "${memPath}"`, WORKSPACE_ROOT).catch(() => {});
    });
    li.appendChild(btn);
    fileListEl.appendChild(li);
  }
}

function buildCustomCommands() {
  const kvCmd = defineCommand('kv', async (args) => {
    const sub = args[0] || 'list';
    try {
      if (sub === 'get') {
        if (!args[1]) return { stdout: '', stderr: 'usage: kv get <key>\n', exitCode: 2 };
        const res = await window.ladybirdApp.kv.get(args[1]);
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'set') {
        if (!args[1] || args[2] === undefined) {
          return { stdout: '', stderr: 'usage: kv set <key> <json-or-text>\n', exitCode: 2 };
        }
        const raw = args.slice(2).join(' ');
        let val = raw;
        try {
          val = JSON.parse(raw);
        } catch (e) {}
        const res = await window.ladybirdApp.kv.set(args[1], val);
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'delete' || sub === 'rm') {
        if (!args[1]) return { stdout: '', stderr: 'usage: kv delete <key>\n', exitCode: 2 };
        const res = await window.ladybirdApp.kv.delete(args[1]);
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'list' || sub === 'ls') {
        const res = await window.ladybirdApp.kv.list(args[1]);
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'clear') {
        const res = await window.ladybirdApp.kv.clear(args[1]);
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      return { stdout: '', stderr: `kv: unknown subcommand '${sub}'\n`, exitCode: 2 };
    } catch (err) {
      return { stdout: '', stderr: `kv error: ${err.message}\n`, exitCode: 1 };
    }
  });

  const accountsCmd = defineCommand('accounts', async (args) => {
    const sub = args[0] || 'list';
    try {
      if (sub === 'current') {
        const res = await window.ladybirdApp.accounts.current();
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'list' || sub === 'ls') {
        const res = await window.ladybirdApp.accounts.list();
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'create') {
        if (!args[1]) return { stdout: '', stderr: 'usage: accounts create <id> [label]\n', exitCode: 2 };
        const res = await window.ladybirdApp.accounts.create(args[1], args[2]);
        await refreshAccounts();
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      if (sub === 'launch') {
        if (!args[1]) return { stdout: '', stderr: 'usage: accounts launch <id>\n', exitCode: 2 };
        const res = await window.ladybirdApp.accounts.launch(args[1], { createIfMissing: true });
        return { stdout: JSON.stringify(res, null, 2) + '\n', stderr: '', exitCode: 0 };
      }
      return { stdout: '', stderr: `accounts: unknown subcommand '${sub}'\n`, exitCode: 2 };
    } catch (err) {
      return { stdout: '', stderr: `accounts error: ${err.message}\n`, exitCode: 1 };
    }
  });

  const sysinfoCmd = defineCommand('sysinfo', async () => {
    const info = await window.ladybirdApp.system.info();
    return { stdout: JSON.stringify(info, null, 2) + '\n', stderr: '', exitCode: 0 };
  });

  return [kvCmd, accountsCmd, sysinfoCmd];
}

async function runBashCommand(command, cwd = WORKSPACE_ROOT) {
  const trimmed = String(command || '').trim();
  if (!trimmed) {
    return { stdout: '', stderr: '', exitCode: 0, synced: { fileCount: syncedSnapshot.size, written: 0, deleted: 0 } };
  }

  let output = null;
  execQueue = execQueue.then(async () => {
    const res = await bash.exec(trimmed, { cwd });
    const synced = await syncWorkspaceToDisk();
    appendTerminal(trimmed, res.stdout || '', res.stderr || '', res.exitCode);
    output = {
      stdout: res.stdout || '',
      stderr: res.stderr || '',
      exitCode: res.exitCode,
      synced,
    };
  });
  await execQueue;
  return output;
}

async function refreshAccounts() {
  const data = await window.ladybirdApp.accounts.list();
  accountSelectEl.innerHTML = '';
  for (const acc of data.accounts || []) {
    const opt = document.createElement('option');
    opt.value = acc.id;
    opt.textContent = acc.active ? `${acc.label} (${acc.id}) [active]` : `${acc.label} (${acc.id})`;
    if (acc.active) opt.selected = true;
    accountSelectEl.appendChild(opt);
  }
  return data;
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

  ctx.registerTool({
    name: 'run_bash',
    description:
      'Execute a sandboxed Bash command inside just-bash over /workspace (synced with Ladybird App FS) with jq, awk, sed, grep, rg, plus Ladybird commands: kv, accounts, sysinfo.',
    inputSchema: {
      type: 'object',
      properties: {
        command: { type: 'string', description: 'Bash command line or pipeline' },
        cwd: { type: 'string', description: 'Working directory (default: /workspace)' },
      },
      required: ['command'],
      additionalProperties: false,
    },
    execute: async ({ command, cwd }) => runBashCommand(command, cwd || WORKSPACE_ROOT),
  });

  ctx.registerTool({
    name: 'get_workbench_info',
    description: 'Return active account, stable origin, workspace file list, and SQLite KV key list.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    execute: async () => {
      const [sys, accountsList, kvList] = await Promise.all([
        window.ladybirdApp.system.info(),
        window.ladybirdApp.accounts.list(),
        window.ladybirdApp.kv.list(),
      ]);
      return {
        origin: window.location.origin,
        system: sys,
        account: activeAccount,
        accounts: accountsList.accounts || [],
        workspaceFiles: Array.from(syncedSnapshot.keys()).sort(),
        kvKeys: (kvList.keys || []).map((k) => k.key),
      };
    },
  });

  webmcpStatusEl.textContent = 'WebMCP: run_bash + get_workbench_info ready';
}

cmdFormEl.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const cmd = cmdInputEl.value.trim();
  if (!cmd) return;
  cmdInputEl.value = '';
  commandHistory.push(cmd);
  if (commandHistory.length > 50) commandHistory = commandHistory.slice(-50);
  historyIndex = commandHistory.length;
  if (window.ladybirdApp.kv) {
    window.ladybirdApp.kv.set('ui:history', commandHistory).catch(() => {});
  }
  await runBashCommand(cmd, WORKSPACE_ROOT).catch((err) => appendTerminal(cmd, '', err.message, 1));
});

cmdInputEl.addEventListener('keydown', (ev) => {
  if (ev.key === 'ArrowUp' && commandHistory.length > 0) {
    ev.preventDefault();
    historyIndex = Math.max(0, historyIndex - 1);
    cmdInputEl.value = commandHistory[historyIndex] || '';
  } else if (ev.key === 'ArrowDown' && commandHistory.length > 0) {
    ev.preventDefault();
    historyIndex = Math.min(commandHistory.length, historyIndex + 1);
    cmdInputEl.value = commandHistory[historyIndex] || '';
  }
});

for (const btn of document.querySelectorAll('.preset-btn')) {
  btn.addEventListener('click', () => {
    const cmd = btn.getAttribute('data-cmd') || '';
    runBashCommand(cmd, WORKSPACE_ROOT).catch((err) => appendTerminal(cmd, '', err.message, 1));
  });
}

document.getElementById('refresh-files-btn').addEventListener('click', () => {
  syncWorkspaceToDisk().catch(() => {});
});

document.getElementById('open-account-btn').addEventListener('click', async () => {
  const id = accountSelectEl.value;
  if (!id || (activeAccount && id === activeAccount.id)) return;
  await window.ladybirdApp.accounts.launch(id, { createIfMissing: false });
});

document.getElementById('new-account-btn').addEventListener('click', async () => {
  const raw = window.prompt('New account slug (lowercase letters, digits, -, _):', 'work');
  if (!raw) return;
  const id = raw.trim().toLowerCase();
  const label = window.prompt('Display label:', id) || id;
  await window.ladybirdApp.accounts.create(id, label.trim());
  await refreshAccounts();
  await window.ladybirdApp.accounts.launch(id, { createIfMissing: true });
});

async function boot() {
  if (!window.ladybirdApp) {
    throw new Error('Ladybird App Mode bridge missing');
  }

  const sys = await window.ladybirdApp.system.info();
  activeAccount = await window.ladybirdApp.accounts.current();
  accountPillEl.textContent = `${activeAccount.label} (${activeAccount.id})`;
  await refreshAccounts();

  memFs = new InMemoryFs();
  await hydrateDirFromHost('workspace', WORKSPACE_ROOT);
  if (syncedSnapshot.size === 0) {
    await seedStarterWorkspace();
  }

  bash = new Bash({
    fs: memFs,
    cwd: WORKSPACE_ROOT,
    customCommands: buildCustomCommands(),
    env: {
      HOME: WORKSPACE_ROOT,
      LADYBIRD_APP_ID: sys.id,
      LADYBIRD_ACCOUNT_ID: activeAccount.id,
      LADYBIRD_PROFILE: activeAccount.profileName,
    },
  });

  if (window.ladybirdApp.kv) {
    const histEntry = await window.ladybirdApp.kv.get('ui:history').catch(() => null);
    if (histEntry && histEntry.found && Array.isArray(histEntry.value)) {
      commandHistory = histEntry.value;
      historyIndex = commandHistory.length;
    }
  }

  await syncWorkspaceToDisk();
  storageMetaEl.textContent = `Origin :${sys.port}${sys.stablePort ? ' (stable)' : ''} · SQLite KV ready`;
  runtimeMetaEl.textContent = `Profile ${activeAccount.profileName} · ${sys.mcpPort ? `MCP :${sys.mcpPort}` : 'MCP: disabled'}`;
  if (!sys.mcpPort) {
    setTimeout(() => {
      window.ladybirdApp.system.info().then((latest) => {
        if (latest && latest.mcpPort) {
          runtimeMetaEl.textContent = `Profile ${activeAccount.profileName} · MCP :${latest.mcpPort}`;
        }
      }).catch(() => {});
    }, 400);
  }

  window.addEventListener('ladybird:webmcp-ready', registerWebMCPTools);
  registerWebMCPTools();

  bootStatusEl.dataset.state = 'ready';
  bootStatusEl.textContent = 'ready (just-bash loaded)';
  appendTerminal(
    null,
    `just-bash initialized in ${WORKSPACE_ROOT} (${syncedSnapshot.size} files) for account '${activeAccount.id}'.`,
    '',
    0
  );
}

boot().catch((err) => {
  bootStatusEl.dataset.state = 'error';
  bootStatusEl.textContent = `boot-error: ${err.message}`;
  appendTerminal(null, '', `Boot failed: ${err.stack || err.message}`, 1);
});
