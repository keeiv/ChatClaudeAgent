// Account normalization must not invent subscription tiers or absent quota values.
'use strict';
const assert = require('node:assert/strict');
const { normalize } = require('../tools/gemini_account.cjs');
for (const tier of [{ id: 'free-tier', name: 'Free' }, { id: 'google-ai-pro', name: 'Google AI Pro' }, { id: 'future-tier', name: 'Future' }]) {
  const result = normalize({ currentTier: tier }, { buckets: [{ modelId: 'official-model', remainingFraction: 0.32, resetTime: '2026-10-27T00:00:00Z' }] });
  assert.equal(result.plan, tier.id);
  assert.equal(result.planLabel, tier.name);
  assert.equal(result.buckets[0].remainingFraction, 0.32);
}
assert.equal(normalize({ currentTier: { id: 'free' }, paidTier: { id: 'paid', name: 'Paid' } }, {}).plan, 'paid');
assert.equal(normalize({}, {}).planLabel, '方案未知');
assert.equal(normalize({}, { buckets: [{}] }).buckets[0].remainingFraction, null);
assert.equal(normalize({}, { buckets: [{}] }).buckets[0].remainingAmount, null);
assert.equal(normalize({}, { buckets: [{ remainingFraction: 0, remainingAmount: '0' }] }).buckets[0].remainingFraction, 0);
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { query } = require('../tools/gemini_account.cjs');
(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'cca-gemini-account-'));
  const write = (name, text) => {
    const file = path.join(root, name); fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, text);
  };
  assert.equal((await query(path.join(root, 'gemini.cmd'), root)).state, 'loggedOut');
  write('.gemini/settings.json', JSON.stringify({ security: { auth: { selectedType: 'oauth-personal' } } }));
  write('.gemini/google_accounts.json', JSON.stringify({ active: 'fixture@example.com' }));
  const cli = 'node_modules/@google/gemini-cli/';
  const core = 'node_modules/@google/gemini-cli-core/';
  write(cli + 'package.json', JSON.stringify({ name: '@google/gemini-cli', version: 'fixture-version', main: 'index.js' }));
  write(cli + 'index.js', '');
  write(core + 'package.json', JSON.stringify({ name: '@google/gemini-cli-core', type: 'module', main: 'index.js' }));
  write(core + 'index.js', '');
  write(core + 'code_assist/oauth2.js', `export async function getOauthClient(type, config) {
    if (type !== 'oauth-personal' || config.isInteractive() || !config.isBrowserLaunchSuppressed()) throw new Error('unsafe login');
    return {};
  }`);
  write(core + 'code_assist/server.js', `export class CodeAssistServer {
    async loadCodeAssist() { return { currentTier: { id: 'free-tier', name: 'Free' }, cloudaicompanionProject: 'fixture-project' }; }
    async retrieveUserQuota({ project }) {
      if (project !== 'fixture-project') throw new Error('wrong project');
      return { buckets: [{ modelId: 'official-model', remainingFraction: 0.4 }] };
    }
  }`);
  const result = await query(path.join(root, 'gemini.cmd'), root);
  assert.equal(result.state, 'ok');
  assert.equal(result.planLabel, 'Free');
  assert.equal(result.version, 'fixture-version');
  assert.equal(result.email, 'fixture@example.com');
  assert.equal(result.buckets[0].remainingFraction, 0.4);
  const bundled = path.join(root, 'bundled');
  const bundleWrite = (name, text) => {
    const file = path.join(bundled, name); fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, text);
  };
  bundleWrite('.gemini/settings.json', JSON.stringify({ security: { auth: { selectedType: 'oauth-personal' } } }));
  bundleWrite(cli + 'package.json', JSON.stringify({ name: '@google/gemini-cli', type: 'module', version: '0.65-fixture',
    bin: { gemini: 'bundle/gemini.js' } }));
  bundleWrite(cli + 'bundle/gemini.js', "import './dist-active.js'; throw new Error('CLI entrypoint must never execute');");
  bundleWrite(cli + 'bundle/core-stale.js', "throw new Error('stale chunk must never execute');");
  bundleWrite(cli + 'bundle/dist-active.js', [
    "export async function getOauthClient(type, config) {",
    " if (type !== 'oauth-personal' || config.isInteractive() || !config.isBrowserLaunchSuppressed()) throw new Error('interactive');",
    " if (process.env.CCA_GEMINI_FIXTURE === 'expired') throw Object.assign(new Error('fixture-secret'), {code:'invalid_grant'});",
    " if (process.env.CCA_GEMINI_FIXTURE === 'network') throw Object.assign(new Error('fixture-secret'), {code:'ETIMEDOUT'});",
    " return {}; }",
    "export class CodeAssistServer {",
    " constructor(client, project) { this.project = project; }",
    " async loadCodeAssist(request) { if (process.env.CCA_GEMINI_FIXTURE === 'service') throw Object.assign(new Error('fixture-secret'), {status:403});",
    " if (process.env.CCA_GEMINI_FIXTURE === 'retired' && !request.mode) return {allowedTiers:[{id:'standard-tier'}],",
    "   ineligibleTiers:[{tierId:'free-tier',reasonCode:'UNSUPPORTED_CLIENT',reasonMessage:'fixture-secret'}]};",
    " return {currentTier:{id:'free',name:'Free'},cloudaicompanionProject:'project'}; }",
    " async retrieveUserQuota({ project }) { if (process.env.CCA_GEMINI_FIXTURE === 'quota') throw new Error('fixture-secret');",
    " if (process.env.CCA_GEMINI_FIXTURE === 'license') throw Object.assign(new Error('fixture-secret'), {response:{status:403}});",
    " if (this.project !== project) throw new Error('quota server must be bound to the project');",
    " return {buckets:[{modelId:'fixture',remainingFraction:0}]}; } }",
  ].join('\n'));
  const bundledExe = path.join(bundled, 'gemini.cmd');
  const bundledResult = await query(bundledExe, bundled);
  assert.equal(bundledResult.state, 'ok');
  assert.equal(bundledResult.version, '0.65-fixture');
  assert.equal(bundledResult.buckets[0].remainingFraction, 0);
  const previous = process.env.CCA_GEMINI_FIXTURE;
  try {
    for (const [scenario, state, phase] of [
      ['expired', 'loggedOut', 'authentication'], ['network', 'error', 'authentication'],
      ['service', 'serviceError', 'service'], ['quota', 'ok', undefined], ['license', 'ok', undefined],
      ['retired', 'retired', undefined],
    ]) {
      process.env.CCA_GEMINI_FIXTURE = scenario;
      const response = await query(bundledExe, bundled);
      assert.equal(response.state, state);
      assert.equal(response.phase, phase);
      assert.ok(!JSON.stringify(response).includes('fixture-secret'));
      if (scenario === 'quota' || scenario === 'retired') assert.ok(response.quotaError);
      if (scenario === 'license') assert.match(response.quotaError, /授權/);
      if (scenario === 'retired') assert.match(response.message, /2026-06-18/);
    }
  } finally {
    if (previous === undefined) delete process.env.CCA_GEMINI_FIXTURE; else process.env.CCA_GEMINI_FIXTURE = previous;
  }
  const missing = path.join(root, 'no-package');
  fs.mkdirSync(path.join(missing, '.gemini'), { recursive: true });
  fs.writeFileSync(path.join(missing, '.gemini/settings.json'), JSON.stringify({ security: { auth: { selectedType: 'oauth-personal' } } }));
  const incompatible = await query(path.join(missing, 'gemini.cmd'), missing);
  assert.equal(incompatible.state, 'error');
  assert.equal(incompatible.phase, 'installation');
  fs.writeFileSync(path.join(missing, '.gemini/settings.json'), JSON.stringify({ security: { auth: { selectedType: 'gemini-api-key' } } }));
  assert.equal((await query(path.join(missing, 'gemini.cmd'), missing)).state, 'wrongMethod');
  console.log('Gemini account SDK discovery and normalization checks passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
