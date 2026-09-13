// One rendered workspace, independent documents and Blockly event histories.
// Session autosave never writes over a user's exported program file.
window.ForeverProgramTabs = (() => {
  const clone = value => JSON.parse(JSON.stringify(value));
  const frame = () => new Promise(resolve => {
    const timer = setTimeout(resolve, 50);
    requestAnimationFrame(() => setTimeout(() => { clearTimeout(timer); resolve(); }, 0));
  });
  function chooseDialog(dialog) {
    return new Promise(resolve => {
      const handlers = new AbortController(), options = {signal: handlers.signal};
      const finish = value => { handlers.abort(); dialog.close(value); resolve(value); };
      dialog.querySelector('form').addEventListener('submit', event => {
        event.preventDefault(); finish(event.submitter?.value || 'cancel');
      }, options);
      dialog.addEventListener('cancel', event => { event.preventDefault(); finish('cancel'); }, options);
      dialog.addEventListener('close', () => finish(dialog.returnValue || 'cancel'), options);
      dialog.returnValue = '';
      dialog.showModal();
    });
  }

  function create({workspace, bridge, load, activated, collectUi, restoreUi}) {
    let documents = [], activeId = '', busy = false, loading = false;
    let timer, epoch = 0, savedText = '', saveSerial = 0, lastError = '', inFlight = 0;
    const closed = [];
    const strip = document.getElementById('programTabs');
    const status = document.getElementById('autosaveStatus');
    const noticeBox = document.getElementById('programNotice');
    const notice = text => { noticeBox.querySelector('span').textContent = text; noticeBox.hidden = !text; };
    noticeBox.querySelector('button').addEventListener('click', () => notice(''));
    const active = () => documents.find(doc => doc.id === activeId);
    const invoke = (method, ...args) => new Promise(resolve => bridge[method](...args, resolve));
    const serialize = () => Blockly.serialization.workspaces.save(workspace);
    const message = (text, error = false) => {
      status.textContent = text; status.title = text;
      status.classList.toggle('error', error);
    };
    function capture() {
      const doc = active();
      if (!doc || loading) return;
      doc.ui = collectUi();
      if (doc.loadError) return; // Preserve source we cannot render, never an empty substitute.
      doc.workspace = serialize();
      doc.undo = workspace.getUndoStack().map(event => event.toJson());
      doc.redo = workspace.getRedoStack().map(event => event.toJson());
      doc.dirty = !doc.path || JSON.stringify(doc.workspace) !== doc.fileState ||
        doc.title !== doc.fileTitle || Object.keys(doc.ui.drafts || {}).length > 0;
    }
    async function save() {
      clearTimeout(timer);
      capture();
      const text = JSON.stringify({version: 2, active: activeId, tabs: documents});
      if (text === savedText && !lastError && !inFlight) {
        message(documents.length ? 'All tabs autosaved' : 'No open programs'); return '';
      }
      const serial = ++saveSerial;
      ++inFlight;
      message('Saving…');
      const error = await invoke('storeSession', text);
      --inFlight;
      if (serial === saveSerial) {
        lastError = error || '';
        if (error) message(error, true);
        else { savedText = text; message(documents.length ? 'All tabs autosaved' : 'No open programs'); }
      }
      render();
      return error || '';
    }
    function changed() {
      if (loading) return;
      ++epoch;
      render();
      clearTimeout(timer);
      message('Saving…');
      timer = setTimeout(save, 160);
    }
    async function settle() {
      // Commit field editors and let Blockly finish its frame-batched event
      // groups before transferring the undo stacks to another document.
      document.activeElement?.blur();
      Blockly.hideChaff();
      let previous;
      do { previous = epoch; await frame(); await frame(); } while (previous !== epoch);
    }
    function render() {
      const ids = new Set(documents.map(doc => doc.id));
      for (const tab of [...strip.children]) if (!ids.has(tab.dataset.document)) tab.remove();
      for (const doc of documents) {
        let tab = [...strip.children].find(item => item.dataset.document === doc.id);
        if (!tab) {
          tab = document.createElement('div'); tab.className = 'programTab'; tab.dataset.document = doc.id;
          const select = document.createElement('button'); select.className = 'programTabSelect'; select.type = 'button';
          select.setAttribute('role', 'tab'); select.id = 'tab-' + doc.id; select.setAttribute('aria-controls', 'editorBody');
          select.addEventListener('click', () => switchTo(doc.id));
          const close = document.createElement('button'); close.className = 'programTabClose'; close.type = 'button'; close.textContent = '×';
          close.addEventListener('click', () => closeTab(doc.id));
          tab.append(select, close); strip.append(tab);
        }
        const [select, close] = tab.children;
        select.textContent = doc.title;
        tab.classList.toggle('dirty', Boolean(doc.dirty));
        select.title = doc.path || 'Autosaved in this session; not saved to a file';
        select.setAttribute('aria-selected', String(doc.id === activeId));
        select.tabIndex = doc.id === activeId ? 0 : -1;
        select.disabled = busy || Boolean(bridge.running);
        close.setAttribute('aria-label', 'Close ' + doc.title);
        close.title = 'Close ' + doc.title; close.disabled = busy || Boolean(bridge.running);
      }
      const doc = active(), locked = busy || Boolean(bridge.running) || !doc || Boolean(doc.loadError);
      document.getElementById('undoProgram').disabled = locked || !workspace.getUndoStack().length;
      document.getElementById('redoProgram').disabled = locked || !workspace.getRedoStack().length;
      for (const button of document.querySelectorAll('[data-project="rename"], [data-project="close"], [data-project="save"], [data-project="save-as"], [data-project="library"]'))
        button.disabled = busy || Boolean(bridge.running) || !doc;
      document.querySelector('[data-project="reopen"]').disabled = busy || Boolean(bridge.running) || !closed.length;
      document.getElementById('editorShell').dataset.empty = String(!doc);
      document.getElementById('newProgramTab').disabled = busy || Boolean(bridge.running);
      if (doc) document.getElementById('editorBody').setAttribute('aria-labelledby', 'tab-' + doc.id);
    }
    async function navigate(action) {
      if (busy || bridge.running) return false;
      busy = true; render();
      try {
        await settle(); capture();
        await action();
        return true;
      } catch (error) {
        notice('Program operation failed: ' + error.message); return false;
      } finally { busy = false; render(); }
    }
    async function show(doc) {
      loading = true;
      notice('');
      let historyWarning = '';
      try {
        activeId = doc?.id || '';
        workspace.clearUndo();
        doc && delete doc.loadError;
        try {
          load(doc ? doc.workspace : {blocks: {languageVersion: 0, blocks: []}});
        } catch (error) {
          if (doc) doc.loadError = error.message;
          workspace.clearUndo();
          notice('This tab could not be loaded. Its original source is still retained: ' + error.message);
        }
        if (doc && !doc.loadError) {
          try {
            const undo = doc.undo.map(event => Blockly.Events.fromJson(event, workspace));
            const redo = doc.redo.map(event => Blockly.Events.fromJson(event, workspace));
            workspace.getUndoStack().push(...undo);
            workspace.getRedoStack().push(...redo);
          } catch (error) {
            // Undo/redo is disposable editor metadata. A stale Blockly event
            // must never make valid program source unloadable.
            workspace.clearUndo();
            doc.undo = [];
            doc.redo = [];
            historyWarning = 'Undo/redo history could not be restored. The program source was loaded safely.';
          }
        }
        activated(doc);
        restoreUi(doc?.ui || {});
      } finally { loading = false; }
      if (historyWarning) notice(historyWarning);
      render();
      strip.querySelector('[aria-selected="true"]')?.scrollIntoView({block: 'nearest', inline: 'nearest'});
      await save();
    }
    async function switchTo(id) {
      if (id === activeId) return;
      const doc = documents.find(item => item.id === id);
      if (doc) await navigate(() => show(doc));
    }
    async function open(state, title = 'Untitled', path = '', view = 'configure') {
      return navigate(async () => {
        const existing = path && documents.find(doc => doc.path === path);
        if (existing) { await show(existing); return; }
        if (documents.length >= 128) throw Error('Close a program before opening more than 128 tabs.');
        const base = title; let count = 2;
        while (documents.some(doc => doc.title === title)) title = `${base} (${count++})`;
        const doc = {id: Blockly.utils.idGenerator.genUid(), title, path, workspace: clone(state), undo: [], redo: [],
          ui: {view}, dirty: !path, fileState: path ? JSON.stringify(state) : '', fileTitle: path ? title : ''};
        documents.push(doc); await show(doc);
      });
    }
    async function saveFile(saveAs = false) {
      const doc = active();
      if (!doc || bridge.running) return false;
      await settle(); capture();
      if (Object.keys(doc.ui.drafts || {}).length) {
        notice('Finish the edited setting before saving a file. Its draft is preserved by autosave.');
        return false;
      }
      const source = JSON.stringify(doc.workspace);
      const response = await invoke('saveProgramFile', source, saveAs ? '' : doc.path,
        doc.title);
      if (!response) return false;
      const result = JSON.parse(response);
      if (result.error) { notice(result.error); return false; }
      if (result.path) {
        notice('');
        doc.path = result.path; doc.title = result.title; doc.fileState = source; doc.fileTitle = result.title;
        await save(); render(); return true;
      }
      return false;
    }
    async function rename(id = activeId) {
      const doc = documents.find(item => item.id === id);
      if (!doc || busy || bridge.running) return;
      const dialog = document.getElementById('renameProgramDialog'), input = document.getElementById('programName');
      input.value = doc.title;
      const choice = chooseDialog(dialog); input.focus(); input.select();
      if (await choice !== 'rename' || !input.value.trim() || !documents.includes(doc)) return;
      doc.title = input.value.trim().slice(0, 200); changed();
    }
    async function closeTab(id = activeId) {
      const doc = documents.find(item => item.id === id);
      if (!doc || busy || bridge.running) return;
      if (id !== activeId) await switchTo(id);
      await settle(); capture();
      if (doc.dirty || doc.loadError) {
        const dialog = document.getElementById('closeProgramDialog');
        document.getElementById('closeProgramName').textContent = doc.title;
        const choice = await chooseDialog(dialog);
        if (choice === 'save') { if (!await saveFile()) return; }
        else if (choice !== 'close') return;
      }
      await navigate(async () => {
        const index = documents.indexOf(doc);
        closed.push(clone(doc)); if (closed.length > 10) closed.shift();
        documents.splice(index, 1);
        await show(documents[Math.min(index, documents.length - 1)]);
      });
    }
    async function reopen() {
      if (!closed.length) return;
      await navigate(async () => { const doc = closed.pop(); documents.push(doc); await show(doc); });
    }
    async function undo(redo = false) {
      if (busy || bridge.running || !active()) return;
      await settle(); workspace.undo(redo); changed();
    }
    async function flush() { await settle(); return save(); }
    document.getElementById('undoProgram').addEventListener('click', () => undo());
    document.getElementById('redoProgram').addEventListener('click', () => undo(true));
    strip.addEventListener('keydown', event => {
      const index = documents.findIndex(doc => doc.id === activeId);
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % documents.length;
      if (event.key === 'ArrowLeft') next = (index + documents.length - 1) % documents.length;
      if (event.key === 'Home') next = 0;
      if (event.key === 'End') next = documents.length - 1;
      if (next !== undefined) { event.preventDefault(); switchTo(documents[next].id).then(() => strip.querySelector('[aria-selected="true"]')?.focus()); }
    });
    strip.addEventListener('wheel', event => {
      if (strip.scrollWidth <= strip.clientWidth || Math.abs(event.deltaX) > Math.abs(event.deltaY)) return;
      strip.scrollLeft += event.deltaY; event.preventDefault();
    }, {passive: false});
    document.addEventListener('keydown', event => {
      if (!(event.ctrlKey || event.metaKey) || event.altKey || document.querySelector('dialog[open]')) return;
      const key = event.key.toLowerCase();
      if (key === 'tab' && documents.length) {
        event.preventDefault(); const index = documents.findIndex(doc => doc.id === activeId);
        switchTo(documents[(index + (event.shiftKey ? documents.length - 1 : 1)) % documents.length].id);
      } else if (key === 's') { event.preventDefault(); saveFile(event.shiftKey); }
      else if (key === 'w') { event.preventDefault(); closeTab(); }
      else if (key === 't' && event.shiftKey) { event.preventDefault(); reopen(); }
      else if ((key === 'z' || key === 'y') && !['INPUT','TEXTAREA'].includes(event.target.tagName)) {
        event.preventDefault(); event.stopImmediatePropagation(); undo(key === 'y' || event.shiftKey);
      }
    }, true);
    document.addEventListener('input', event => { if (event.target.closest('#configurationCards')) changed(); });
    document.getElementById('autosaveRetry').addEventListener('click', save);
    bridge.sessionFlushRequested.connect(async () => {
      try { bridge.finishSessionFlush(await flush()); }
      catch (error) { bridge.finishSessionFlush(error.message); }
    });
    async function initialize() {
      if (bridge.sessionJson) {
        const session = JSON.parse(bridge.sessionJson);
        documents = session.tabs;
        activeId = session.active;
        await show(active());
      } else {
        await open(JSON.parse(bridge.workspaceJson), 'Untitled');
      }
      if (bridge.sessionError) notice(bridge.sessionError);
    }
    return {initialize, open, switchTo, active, changed, save, flush, render, saveFile, closeTab, rename, reopen,
      get busy() { return busy || loading; }};
  }
  return {create};
})();
