// Query account metadata through the installed official CLI SDK; credentials never leave this process.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { createRequire } = require('node:module');
const { pathToFileURL } = require('node:url');

function normalize(account, quota) {
  const tier = account.paidTier || account.currentTier;
  return {
    state: 'ok', plan: tier?.id || '', planLabel: tier?.name || tier?.id || '方案未知',
    buckets: (Array.isArray(quota?.buckets) ? quota.buckets : []).filter(b => b && typeof b === 'object').slice(0, 100).map(b => ({
      modelId: String(b.modelId || ''), tokenType: String(b.tokenType || ''),
      remainingFraction: typeof b.remainingFraction === 'number' && Number.isFinite(b.remainingFraction)
        ? Math.max(0, Math.min(1, b.remainingFraction)) : null,
      remainingAmount: b.remainingAmount == null ? null : String(b.remainingAmount),
      resetTime: String(b.resetTime || ''),
    })),
  };
}

function cliPackage(exe) {
  const base = path.dirname(path.resolve(exe));
  const candidates = [path.join(base, 'node_modules/@google/gemini-cli/package.json')];
  const requireCli = createRequire(path.join(base, 'cca-gemini.cjs'));
  let real = path.resolve(exe);
  try { real = fs.realpathSync(real); } catch (_) { /* Fixtures may omit the executable shim. */ }
  for (let dir = path.dirname(real), depth = 0; depth < 8; depth++, dir = path.dirname(dir)) {
    candidates.push(path.join(dir, 'package.json'));
  }
  for (const file of candidates) {
    try {
      const info = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (info.name === '@google/gemini-cli') return { root: path.dirname(file), info, requireCli };
    } catch (_) { /* Not the selected CLI package. */ }
  }
  const error = new Error('CLI package unavailable'); error.code = 'CCA_SDK_UNAVAILABLE'; throw error;
}

async function loadSdk(exe) {
  const pkg = cliPackage(exe);
  try {
    let entry;
    try { entry = pkg.requireCli.resolve('@google/gemini-cli-core'); }
    catch (_) { entry = createRequire(path.join(pkg.root, 'package.json')).resolve('@google/gemini-cli-core'); }
    const realEntry = fs.realpathSync(entry);
    const allowed = fs.realpathSync(path.dirname(pkg.root)) + path.sep;
    if (!realEntry.startsWith(allowed)) throw new Error('SDK outside selected installation');
    const load = name => import(pathToFileURL(path.join(path.dirname(entry), name)).href);
    const [oauth, server] = await Promise.all([load('code_assist/oauth2.js'), load('code_assist/server.js')]);
    return { ...pkg, sdk: { ...oauth, ...server } };
  } catch (_) { /* npm 0.65 also ships its SDK as bundled ESM exports. */ }
  const bin = typeof pkg.info.bin === 'string' ? pkg.info.bin : pkg.info.bin?.gemini;
  const queue = bin ? [path.resolve(pkg.root, bin)] : [];
  const seen = new Set();
  const root = fs.realpathSync(pkg.root);
  let bytes = 0;
  while (queue.length && seen.size < 128 && bytes < 128 * 1024 * 1024) {
    let file = queue.shift();
    try { file = fs.realpathSync(file); } catch (_) { continue; }
    if (!file.startsWith(root + path.sep) || seen.has(file)) continue;
    seen.add(file);
    if (fs.statSync(file).size > 32 * 1024 * 1024) continue;
    const source = fs.readFileSync(file, 'utf8');
    bytes += Buffer.byteLength(source);
    // Follow only this installation's active entry graph; stale chunks from older npm updates are ignored.
    if (/^(?:core|dist)-[^/]+\.js$/.test(path.basename(file)) &&
        source.includes('getOauthClient') && source.includes('CodeAssistServer')) {
      const sdk = await import(pathToFileURL(file).href);
      if (typeof sdk.getOauthClient === 'function' && typeof sdk.CodeAssistServer === 'function') return { ...pkg, sdk };
    }
    for (const item of source.matchAll(/['"](\.\/[^'"\s]+\.js)['"]/g)) queue.push(path.resolve(path.dirname(file), item[1]));
  }
  const error = new Error('Compatible SDK exports unavailable'); error.code = 'CCA_SDK_UNAVAILABLE'; throw error;
}

function failure(error, phase, version = '') {
  const code = error?.response?.status || error?.status || error?.code;
  const signedOut = phase === 'authentication' && (code === 401 || code === 'invalid_grant');
  return {
    state: signedOut ? 'loggedOut' : phase === 'service' ? 'serviceError' : 'error', phase, version,
    message: phase === 'installation' ? '已找到 Gemini CLI，但其套件格式不相容；無法載入帳號檢查工具。' :
      signedOut ? 'Google 登入已失效，請在 Gemini CLI 重新登入。' :
        phase === 'authentication' ? '暫時無法驗證 Google 登入，請檢查網路後重試；既有登入資料已保留。' :
          'Google 登入已驗證，但 Gemini 服務尚未通過檢查；請稍後重試或在官方 CLI 確認帳號可用。',
  };
}

async function query(exe, accountHome) {
  let home = path.join(accountHome || process.env.GEMINI_CLI_HOME || os.homedir(), '.gemini');
  let settings = {};
  try { settings = JSON.parse(fs.readFileSync(path.join(home, 'settings.json'), 'utf8')); } catch (_) { /* No settings yet. */ }
  const selected = settings.security?.auth?.selectedType || settings.selectedAuthType;
  if (selected && selected !== 'oauth-personal') {
    return { state: 'wrongMethod', message: 'Gemini CLI 目前使用其他驗證方式；請選 Login with Google。' };
  }
  if (!selected && !fs.existsSync(path.join(home, 'oauth_creds.json')) &&
      !fs.existsSync(path.join(home, 'token-storage.json')) && !fs.existsSync(path.join(home, 'google_accounts.json'))) {
    return { state: 'loggedOut', message: '尚無 Google 登入設定；請先在 Gemini CLI 選 Login with Google。' };
  }
  let loaded;
  try { loaded = await loadSdk(exe); } catch (error) { return failure(error, 'installation'); }
  const { sdk } = loaded, version = loaded.info.version || '';
  if (!accountHome && typeof sdk.Storage?.getGlobalGeminiDir === 'function') home = sdk.Storage.getGlobalGeminiDir();
  let client;
  try {
    client = await sdk.getOauthClient('oauth-personal', {
      getProxy: () => undefined, isBrowserLaunchSuppressed: () => true, getNoBrowser: () => true,
      isInteractive: () => false, getAcpMode: () => false,
    });
  } catch (error) { return failure(error, 'authentication', version); }
  const server = new sdk.CodeAssistServer(client);
  let account;
  try {
    const metadata = typeof sdk.getClientMetadata === 'function' ? await sdk.getClientMetadata() :
      { ideType: 'GEMINI_CLI', pluginType: 'GEMINI', platform: 'WINDOWS_AMD64' };
    account = await server.loadCodeAssist({ metadata, mode: 'HEALTH_CHECK' });
  } catch (error) { return failure(error, 'service', version); }
  if (!account || (!account.currentTier && !account.paidTier)) {
    return { state: 'serviceError', version, message: 'Google 登入已驗證，但尚未回報可用方案；請在官方 Gemini CLI 完成帳號啟用。' };
  }
  let email = '';
  try {
    const accounts = JSON.parse(fs.readFileSync(path.join(home, 'google_accounts.json'), 'utf8'));
    email = typeof accounts.active === 'string' ? accounts.active : '';
  } catch (_) { /* Optional display metadata. */ }
  // A health check still answers for a retired client; only the CLI's own setup request reveals the refusal.
  let setup = null;
  try {
    setup = await server.loadCodeAssist({ metadata: { ideType: 'IDE_UNSPECIFIED', platform: 'PLATFORM_UNSPECIFIED', pluginType: 'GEMINI' } });
  } catch (_) { /* Eligibility stays unknown; the quota request below still reports what it can. */ }
  const ineligible = Array.isArray(setup?.ineligibleTiers) ? setup.ineligibleTiers : [];
  if (setup && !setup.currentTier && ineligible.some(t => t?.reasonCode === 'UNSUPPORTED_CLIENT')) {
    return {
      ...normalize(account, null), state: 'retired', email, version, quotaError: '此帳號無法使用 Gemini CLI，因此沒有剩餘額度。',
      message: 'Google 已於 2026-06-18 停止免費層與 Google One 個人帳號使用 Gemini CLI（官方改為 Antigravity CLI）；這個帳號目前無法執行 Gemini，也查不到剩餘額度。',
    };
  }
  const tier = setup?.paidTier || setup?.currentTier || account.paidTier || account.currentTier;
  const project = setup?.cloudaicompanionProject || account.cloudaicompanionProject;
  let quota, quotaError = '';
  try {
    if (!project) throw new Error('No project');
    // The official CLI asks with a server bound to the project and tier that setup returned.
    const quotaServer = new sdk.CodeAssistServer(client, project, {}, '', tier?.id, tier?.name);
    quota = await quotaServer.retrieveUserQuota({ project });
  } catch (error) {
    const status = error?.response?.status || error?.status;
    quotaError = status === 403 ? 'Google 回報這個帳號沒有 Gemini Code Assist 授權，無法查詢剩餘額度。' : '無法取得 Google 剩餘額度；剩餘量未知。';
  }
  return { ...normalize(setup?.currentTier ? setup : account, quota), email, quotaError, version, message: quotaError ? 'Google 登入已驗證，剩餘額度暫時未知。' : '' };
}

if (require.main === module) {
  const timeout = setTimeout(() => {
    process.stdout.write('CCA_GEMINI_STATUS:' + JSON.stringify({ state: 'error', message: 'Gemini 帳號查詢逾時；請重新檢查。' }) + '\n');
    process.exit(1);
  }, 25000);
  query(process.argv[2]).then(result => {
    clearTimeout(timeout);
    process.stdout.write('CCA_GEMINI_STATUS:' + JSON.stringify(result) + '\n');
    process.exit(0);
  }).catch(() => {
    clearTimeout(timeout);
    // SDK errors can contain credentials or HTTP headers; never forward their raw text.
    process.stdout.write('CCA_GEMINI_STATUS:' + JSON.stringify({ state: 'error', message: 'Gemini 帳號工具未能完成檢查；既有登入資料已保留，請重試或檢查 CLI 相容性。' }) + '\n');
    process.exit(0);
  });
}
module.exports = { normalize, query, loadSdk, cliPackage };
