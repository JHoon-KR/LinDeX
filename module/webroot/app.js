const ctl = '/data/adb/modules/debian_chroot/bin/debian-gpu-control';
const $ = id => document.getElementById(id);
const compositorProfiles = new Set(['sway']);
const profileNameKeys = {sway:'profileSway', lxqt:'profileLxqt', xfce:'profileXfce'};
let language = localStorage.getItem('language') || ((navigator.language || '').startsWith('ko') ? 'ko' : 'en');
let messages = globalThis.PROFILE_I18N[language] || globalThis.PROFILE_I18N.en;
let modes = [];
let busy = Promise.resolve();
let lastState = null;
const t = key => messages[key] || key;
const setupStageKeys = {
  preflight:'stagePreflight', 'base-packages':'stageBasePackages', mesa:'stageMesa',
  'profile-runtime':'stageProfileRuntime', 'profile-dependencies':'stageProfileDependencies',
  'profile-packages':'stageProfilePackages', 'profile-configuration':'stageProfileConfiguration',
  finalizing:'stageFinalizing', complete:'stageComplete', failed:'stageFailed',
  'not-installed':'stageNotInstalled'
};

function applyLanguage() {
  messages = globalThis.PROFILE_I18N[language] || globalThis.PROFILE_I18N.en;
  document.documentElement.lang = language;
  document.querySelectorAll('[data-i18n]').forEach(node => { node.textContent = t(node.dataset.i18n); });
  $('language').value = language;
  updateProfileHelp();
}

function normalize(result) {
  if (typeof result === 'string') return { errno: 0, stdout: result, stderr: '' };
  return result || { errno: 0, stdout: '', stderr: '' };
}

async function rawExec(command) {
  if (window.ksu && typeof window.ksu.exec === 'function') {
    return await new Promise((resolve, reject) => {
      const callback = `profileExec_${Date.now()}_${Math.random().toString(16).slice(2)}`;
      const timer = setTimeout(() => { delete window[callback]; reject(new Error(t('commandFailed'))); }, 30000);
      window[callback] = (errno, stdout, stderr) => { clearTimeout(timer); delete window[callback]; resolve({errno, stdout, stderr}); };
      try {
        const returned = window.ksu.exec(command, '{}', callback);
        if (returned && typeof returned.then === 'function') returned.then(resolve, reject);
      } catch (error) { clearTimeout(timer); delete window[callback]; reject(error); }
    });
  }
  if (typeof window.exec === 'function') return await Promise.resolve(window.exec(command));
  throw new Error(t('noCommandApi'));
}

function command(args) {
  const task = busy.then(async () => {
    const result = normalize(await rawExec(`${ctl} ${args}`));
    if (result.errno) throw new Error([result.stderr, result.stdout].filter(Boolean).join('\n') || t('commandFailed'));
    return result;
  });
  busy = task.catch(() => {});
  return task;
}

function updateProfileHelp() {
  const profile = $('profile').value;
  $('profileHelp').textContent = compositorProfiles.has(profile) ? t('archiveHelp') : t('desktopHelp');
  $('archcraftSupport').hidden = profile !== 'sway';
  $('swayThemeSetting').hidden = profile !== 'sway';
}

async function refreshProfiles() {
  try {
    const result = await command('profiles');
    const profiles = result.stdout.trim().split(/\r?\n\s*\r?\n/).map(block => {
      const fields = Object.fromEntries(block.split(/\r?\n/).filter(Boolean).map(line => {
        const split = line.indexOf('=');
        return split > 0 ? [line.slice(0, split), line.slice(split + 1)] : ['', ''];
      }).filter(([key]) => key));
      return fields.id && fields.name ? fields : null;
    }).filter(Boolean);
    if (!profiles.length) return;
    const selected = $('profile').value;
    $('profile').replaceChildren(...profiles.map(profile => {
      const option = document.createElement('option');
      option.value = profile.id;
      const nameKey = profileNameKeys[profile.id];
      if (nameKey) option.dataset.i18n = nameKey;
      option.textContent = nameKey ? t(nameKey) : profile.name;
      return option;
    }));
    if (profiles.some(profile => profile.id === selected)) $('profile').value = selected;
    updateProfileHelp();
  } catch (_) { /* Keep the embedded release list as an offline fallback. */ }
}

async function setValue(key, value) {
  await command(`set ${key} ${value}`);
  await refreshStatus();
}

async function refreshStatus() {
  try {
    const result = await command('status-json');
    const state = JSON.parse(result.stdout.trim());
    lastState = state;
    const installing = state.setupRunning === true;
    const installFailed = state.preparationState === 'failed' || state.installStage === 'failed';
    const installPercent = Math.max(0, Math.min(100, Number(state.installPercent) || 0));
    $('hardware').textContent = state.hardware === 'ready' ? t('ready') : t('blocked');
    $('display').textContent = state.dp === 'connected' ? t('connected') : state.dp === 'adapter' ? t('adapter') : t('disconnected');
    $('session').textContent = state.running ? t('running') : t('stopped');
    $('preparation').textContent = installing ? `${t('installing')} · ${installPercent}%` :
      installFailed ? t('installFailed') : state.profileInstalled && state.bridgeReady ? t('installed') : t('required');
    $('installProgress').hidden = !(installing || installFailed || !state.profileInstalled);
    $('installProgressBar').value = installPercent;
    $('installProgressValue').textContent = `${installPercent}%`;
    $('installStage').textContent = t(setupStageKeys[state.installStage] || 'stageNotInstalled');
    $('start').setAttribute('aria-disabled', installing ? 'true' : 'false');
    $('profile').disabled = installing;
    $('mesa').disabled = installing;
    $('flavor').textContent = state.buildFlavor === 'dev' ? t('dev') : t('release');
    $('flavor').className = `pill ${state.buildFlavor === 'dev' ? 'warn' : 'ok'}`;
    $('profile').value = state.profile;
    $('swayTheme').value = state.swayTheme || 'dark';
    $('mesa').value = state.mesaMode;
    $('autoAttach').checked = state.autoAttach === 1;
    $('xwayland').checked = state.xwayland === 1;
    $('shareTouch').checked = state.shareTouch === 1;
    $('usbInput').value = state.usbInputMode;
    $('video').value = state.videoAcceleration;
    $('outputModifiers').value = state.outputPolicy;
    $('directScanout').value = state.directScanout;
    $('devPanel').hidden = state.buildFlavor !== 'dev';
    $('message').textContent = state.lastError && state.lastError !== 'none' ? state.lastError : state.preparationDetail;
    updateProfileHelp();
  } catch (error) {
    $('message').textContent = error.message;
  }
}

function fillRefresh(resolution, selected = '') {
  const select = $('refresh');
  select.replaceChildren();
  if (resolution === 'auto') {
    const option = document.createElement('option'); option.textContent = 'Hz'; select.append(option); select.disabled = true; return;
  }
  const matching = modes.filter(mode => mode.resolution === resolution);
  for (const mode of matching) {
    const option = document.createElement('option'); option.value = mode.value;
    option.textContent = `${mode.hz.toFixed(2)} Hz${mode.experimental ? ` · ${t('experimental')}` : ''}${mode.current ? ` · ${t('current')}` : ''}`;
    select.append(option);
  }
  select.disabled = matching.length === 0;
  select.value = matching.some(mode => mode.value === selected) ? selected : matching.find(mode => mode.current)?.value || matching[0]?.value || '';
}

async function refreshModes() {
  try {
    const result = await command('display-modes');
    const lines = result.stdout.split(/\r?\n/);
    const current = lines.find(line => line.startsWith('CURRENT='))?.slice(8) || '';
    modes = lines.filter(line => /^(MODE|EXPERIMENTAL_MODE)=/.test(line)).map(line => {
      const experimental = line.startsWith('EXPERIMENTAL_MODE=');
      const value = line.slice(line.indexOf('=') + 1);
      const [resolution, rawHz] = value.split('@');
      return {value, resolution, hz:Number(rawHz), experimental, current:value === current};
    }).filter(mode => mode.resolution && Number.isFinite(mode.hz));
    const resolutions = [...new Set(modes.map(mode => mode.resolution))];
    const selectedMode = (await command('status-json').then(r => JSON.parse(r.stdout))).displayMode;
    const selectedResolution = selectedMode === 'auto' ? 'auto' : selectedMode.split('@')[0];
    const select = $('resolution');
    select.replaceChildren();
    const automatic = document.createElement('option'); automatic.value='auto'; automatic.textContent=t('automatic'); select.append(automatic);
    for (const resolution of resolutions) { const option=document.createElement('option'); option.value=resolution; option.textContent=resolution; select.append(option); }
    select.value = resolutions.includes(selectedResolution) ? selectedResolution : 'auto';
    fillRefresh(select.value, selectedMode);
  } catch (_) { /* A detached monitor has no live mode list. */ }
}

$('language').addEventListener('change', event => { language=event.target.value; localStorage.setItem('language', language); applyLanguage(); refreshStatus(); });
$('profile').addEventListener('change', event => {
  updateProfileHelp();
  setValue('profile', event.target.value).catch(error => $('message').textContent=error.message);
});
$('swayTheme').addEventListener('change', event => setValue('sway_theme', event.target.value).catch(error => $('message').textContent=error.message));
$('mesa').addEventListener('change', event => setValue('mesa', event.target.value).catch(error => $('message').textContent=error.message));
$('autoAttach').addEventListener('change', event => setValue('auto_attach', event.target.checked ? 1 : 0).catch(error => $('message').textContent=error.message));
$('xwayland').addEventListener('change', event => setValue('xwayland', event.target.checked ? 1 : 0).catch(error => $('message').textContent=error.message));
$('shareTouch').addEventListener('change', event => setValue('share_touch', event.target.checked ? 1 : 0).catch(error => $('message').textContent=error.message));
$('usbInput').addEventListener('change', event => setValue('usb_input', event.target.value).catch(error => $('message').textContent=error.message));
$('video').addEventListener('change', event => setValue('video_acceleration', event.target.value).catch(error => $('message').textContent=error.message));
$('outputModifiers').addEventListener('change', event => setValue('output_modifiers', event.target.value).catch(error => $('message').textContent=error.message));
$('directScanout').addEventListener('change', event => setValue('direct_scanout', event.target.value).catch(error => $('message').textContent=error.message));
$('resolution').addEventListener('change', event => {
  if (event.target.value === 'auto') { fillRefresh('auto'); setValue('display_mode','auto').catch(error => $('message').textContent=error.message); }
  else fillRefresh(event.target.value);
});
$('refresh').addEventListener('change', event => setValue('display_mode',event.target.value).catch(error => $('message').textContent=error.message));
$('start').addEventListener('click', async () => {
  if (lastState?.setupRunning) { $('message').textContent=t('installationInProgress'); return; }
  $('message').textContent=t('starting');
  try { await command('start-ready-async'); await refreshStatus(); }
  catch(error) { $('message').textContent=error.message.includes('INSTALLATION_IN_PROGRESS') ? t('installationInProgress') : error.message; }
});
$('stop').addEventListener('click', async () => { $('message').textContent=t('stopping'); try { await command('stop'); await refreshStatus(); } catch(error) { $('message').textContent=error.message; } });
$('readLog').addEventListener('click', async () => { try { $('devLog').textContent=(await command('dev-log 240')).stdout; } catch(error) { $('devLog').textContent=error.message; } });
$('clearLog').addEventListener('click', async () => { try { await command('clear-dev-logs'); $('devLog').textContent=''; } catch(error) { $('devLog').textContent=error.message; } });

applyLanguage();
refreshProfiles().then(refreshStatus);
refreshModes();
setInterval(() => { if (!document.hidden) refreshStatus(); }, 4000);
setInterval(() => { if (!document.hidden) refreshModes(); }, 12000);
