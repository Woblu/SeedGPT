(() => {
  const $ = s => document.querySelector(s);
  const $$ = s => Array.from(document.querySelectorAll(s));
  const el = (t, c, x) => { const n = document.createElement(t);
    if (c) n.className = c; if (x !== undefined) n.textContent = x;
    // A seed number is clicked to copy it, in a dozen places. As a bare <span>
    // it could not be reached by keyboard at all; made a button here, once,
    // every one of them can (Enter/Space is handled below).
    if (c === 'num' && t === 'span') {
      n.setAttribute('role', 'button'); n.tabIndex = 0;
      n.setAttribute('aria-label', 'copy seed ' + (x || ''));
    }
    return n; };
  document.addEventListener('keydown', e => {
    if ((e.key === 'Enter' || e.key === ' ') && e.target.matches &&
        e.target.matches('span.num[role="button"]')) { e.preventDefault(); e.target.click(); }
  });
  // Something worth saying aloud: progress, a result, a stop. Screen readers
  // otherwise hear nothing while results replace each other on screen.
  const announce = msg => { const s = document.getElementById('srStatus');
    if (s) { s.textContent = ''; setTimeout(() => { s.textContent = msg; }, 30); } };
  const esc = s => String(s).replace(/[&<>"]/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

  // Display-only prettifier: drop the minecraft: prefix and underscores, then
  // Title Case. The raw id is always kept as the <option> value / query field;
  // only the visible text is cleaned up.
  const pretty = s => String(s).replace(/^minecraft:/, '')
    .replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase());

  // ---- icons -------------------------------------------------------------
  // Emoji give instant character with zero downloads; if the user drops a PNG
  // into assets/structures/<id>.png or assets/items/<id>.png it upgrades in
  // place. `assets` records which files actually exist so we never point an
  // <img> at a 404 (which flickers a broken-image glyph).
  let assets = { structures: [], items: [], backdrop: false };

  const STRUCT_EMOJI = {
    mansion:'🏰', woodland_mansion:'🏰', village:'🏘️', monument:'🔱',
    ocean_monument:'🔱', ancient_city:'🏛️', outpost:'🏴', pillager_outpost:'🏴',
    desert_pyramid:'🏜️', jungle_temple:'🛕', jungle_pyramid:'🛕', swamp_hut:'🧙',
    witch_hut:'🧙', igloo:'🧊', ruined_portal:'🌀', ruined_portal_nether:'🌀',
    shipwreck:'⛵', ocean_ruin:'🌊', treasure:'💰', buried_treasure:'💰',
    fortress:'🔥', nether_fortress:'🔥', bastion:'🐷', bastion_remnant:'🐷',
    end_city:'🌃', trail_ruins:'🪨', trial_chambers:'⚔️', mineshaft:'⛏️',
    stronghold:'📕', geode:'💜', fossil:'🦴', desert_well:'🪣', end_gateway:'🌌',
  };
  const ITEM_EMOJI = {
    diamond:'💎', diamond_ore:'💎', emerald:'💚', emerald_ore:'💚',
    gold_ingot:'🟡', gold_nugget:'🟡', golden_apple:'🍎',
    enchanted_golden_apple:'🍏', iron_ingot:'⬜', iron_nugget:'⬜',
    saddle:'🪮', name_tag:'🏷️', bell:'🔔', book:'📖', enchanted_book:'📚',
    trident:'🔱', heart_of_the_sea:'💙', nautilus_shell:'🐚', tnt:'🧨',
    music_disc:'💿', bone:'🦴', arrow:'🏹', bread:'🍞', wheat:'🌾',
    apple:'🍎', obsidian:'⬛', ender_pearl:'🟣', gunpowder:'💥', string:'🧵',
    redstone:'🔴', lapis:'🔵', copper:'🟠', coal:'⚫', quartz:'⬜',
    ancient_debris:'🟤', nether_gold:'🟡',
  };
  function biomeEmoji(name) {
    const n = String(name || '');
    const rules = [['mushroom','🍄'],['desert','🏜️'],['badlands','🧱'],
      ['mesa','🧱'],['jungle','🌴'],['bamboo','🎋'],['swamp','🐸'],
      ['mangrove','🌿'],['ocean','🌊'],['beach','🏖️'],['river','💧'],
      ['ice','❄️'],['frozen','❄️'],['snow','⛄'],['taiga','🌲'],
      ['spruce','🌲'],['savanna','🦁'],['cherry','🌸'],['flower','🌷'],
      ['sunflower','🌻'],['dark_forest','🌳'],['forest','🌳'],['birch','🪵'],
      ['plains','🌾'],['meadow','🌼'],['mountain','⛰️'],['peak','🏔️'],
      ['hills','⛰️'],['grove','🌲'],['cave','🕳️'],['nether','🔥'],
      ['basalt','🌋'],['soul','💀'],['crimson','🍄'],['warped','🌀'],
      ['end','🌌'],['void','⬛'],['mangrove','🌿']];
    for (const [k, e] of rules) if (n.includes(k)) return e;
    return '🌱';
  }
  function emojiFor(cat, name) {
    if (cat === 'eyes')   return '👁️';
    if (cat === 'slime')  return '🟢';
    if (cat === 'height') return '⛰️';
    if (cat === 'spawn')  return '🧭';
    if (cat === 'biome')  return biomeEmoji(name);
    if (cat === 'item')   return ITEM_EMOJI[name] || '📦';
    return STRUCT_EMOJI[name] || '📍';   // structure / place
  }
  function mkIcon(cat, name) {
    const s = el('span', 'ic');
    if (cat === 'structure' && assets.structures.includes(name)) {
      const im = el('img'); im.src = '/assets/structures/' + name + '.png';
      im.alt = ''; s.append(im); return s;
    }
    if (cat === 'item' && assets.items.includes(name)) {
      const im = el('img'); im.src = '/assets/items/' + name + '.png';
      im.alt = ''; s.append(im); return s;
    }
    s.textContent = emojiFor(cat, name);
    return s;
  }

  // A table cell with an icon + label. Shared by search results and the village
  // search, so it lives here rather than inside either handler.
  function idCell(cat, name, label) {
    const td = el('td', 'id');
    td.append(mkIcon(cat, name), document.createTextNode(label));
    return td;
  }

  let vocab = { structures:{}, biomes:{} }, lootVocab = { structures:{} };
  let conds = [], nextId = 1;

  // ---- history sidebar + shell -------------------------------------------
  const HIST_LS = 'sf-history';
  const getHistory = () => { try { return JSON.parse(localStorage.getItem(HIST_LS) || '[]'); } catch (_) { return []; } };
  function addHistory(text) {
    let h = getHistory().filter(x => x.text !== text);
    h.unshift({ text, ts: Date.now() });
    localStorage.setItem(HIST_LS, JSON.stringify(h.slice(0, 60)));
    renderHistory();
  }
  const setHistory = h => { localStorage.setItem(HIST_LS, JSON.stringify(h)); renderHistory(); };
  // Attach the query a prompt resolved to, so the entry can be replayed.
  function rememberQuery(text, query, notes) {
    const h = getHistory(), it = h.find(x => x.text === text);
    if (!it) return;
    it.query = JSON.parse(JSON.stringify(query)); delete it.query.budget; delete it.query.tool;
    it.notes = notes || '';
    try { setHistory(h); } catch (_) {}      // storage full: keep working without it
  }
  function renderHistory() {
    const box = $('#history'); box.innerHTML = '';
    const h = getHistory();
    $('#histClear').disabled = !h.length;
    if (!h.length) { box.append(el('div', 'hist-empty', 'Your searches show up here.')); return; }
    h.forEach((item, i) => {
      const row = el('div', 'hist-row');
      const b = el('button', 'hist-item', item.text);
      // A remembered entry re-runs the query the AI produced last time --
      // exactly, instantly and without another AI call. Re-asking cost a
      // request and could come back as a DIFFERENT query.
      b.title = item.query ? item.text + ' — re-runs the same query, no AI call' : item.text;
      b.onclick = () => {
        closeSidebar();
        if (item.query) {
          $('#nl').value = ''; aiNote(item.notes || '');
          runSearch(JSON.parse(JSON.stringify(item.query)));
        } else { $('#nl').value = item.text; submitPrompt(); }
      };
      // A history you cannot prune is a history you stop reading.
      const x = el('button', 'hist-del', '✕');
      x.title = 'forget this one';
      x.onclick = () => { const cur = getHistory(); cur.splice(i, 1); setHistory(cur); };
      row.append(b, x);
      box.append(row);
    });
  }
  $('#histClear').onclick = () => {
    if (getHistory().length && confirm('Forget every past search?')) setHistory([]);
  };
  renderHistory();

  $('#newSearch').onclick = () => {
    $('#nl').value = ''; $('#nl').style.height = 'auto'; aiNote('');
    out.innerHTML = ''; stopScroll();
    $('#main').classList.remove('searched'); closeSidebar();
    $('#nl').focus();
  };

  // Sidebar collapses under a narrow viewport; the ☰ button toggles it.
  const closeSidebar = () => { $('#app').classList.remove('sb-open'); $('#scrim').classList.add('hidden'); };
  $('#menuBtn').onclick = () => {
    const open = $('#app').classList.toggle('sb-open');
    $('#scrim').classList.toggle('hidden', !open);
  };

  // ---- vocabulary --------------------------------------------------------
  async function loadVocab() {
    const v = $('#version').value;
    try {
      const r = await fetch('/api/vocab?v=' + encodeURIComponent(v));
      const d = await r.json();
      if (!r.ok) throw new Error(d.error);
      vocab = d;
      try {
        const lr = await fetch('/api/lootitems?v=' + encodeURIComponent(v));
        lootVocab = await lr.json();
      } catch (e) { lootVocab = { structures:{} }; }
      const line = `cubiomes · MC ${d.version} · ${Object.keys(d.structures).length} structures · ${Object.keys(d.biomes).length} biomes`;
      $('#engine').textContent = line;
      $('#engineInfo').textContent = line;
      vocabTries = 0;
      syncEdition();
      // Drop conditions this version can't express, rather than failing later.
      // Only structure/biome names are version-scoped; ore, eyes, loot and slime
      // (slime placement is version-independent) carry across any version.
      const before = conds.length;
      conds = conds.filter(c =>
        c.kind === 'structure'            ? c.name in vocab.structures :
        c.kind === 'biome' || c.kind === 'area' ? c.name in vocab.biomes : true);
      render();
      if (conds.length !== before) toast(`${before - conds.length} condition(s) not in MC ${d.version} — removed`);
    } catch (e) {
      $('#engine').textContent = 'vocab failed: ' + e.message;
      $('#engineInfo').textContent = 'vocab failed: ' + e.message;
      // Without the vocabulary the builder has no structure or biome to offer,
      // and nothing used to try again. A server still starting up is the
      // usual cause, so a few spaced retries fix it without a reload.
      if (++vocabTries <= 4) setTimeout(loadVocab, 1500 * vocabTries);
      else toast('could not load the structure list from the server — reload the page');
    }
  }
  let vocabTries = 0;
  $('#version').onchange = loadVocab;
  // Edition, version and world type, always visible next to the prompt. The
  // version lived only in the Build drawer, and the page never said "Java" --
  // which matters, because Bedrock structures do not match Java's (measured
  // 0/14), so a Java seed is not a Bedrock seed.
  function syncEdition() {
    const v = (vocab && vocab.version) || $('#version').value;
    $('#edVer').textContent = v;
    $('#edLarge').textContent = largeBiomes() ? ' · Large Biomes' : '';
  }
  $('#edChange').onclick = () => { openDrawer('build'); setTimeout(() => $('#version').focus(), 0); };
  $('#largeBiomes').addEventListener('change', syncEdition);

  // Toasts float over the page. They used to be prepended into the results
  // column, which on the landing screen is empty and well below the fold --
  // so the sentence explaining why nothing happened was never read.
  function toast(msg, kind) {
    const n = el('div', 'note ' + (kind || 'warn'), msg);
    $('#toasts').append(n);
    setTimeout(() => n.remove(), 5200);
  }

  // ---- conditions --------------------------------------------------------
  function addCond(kind) {
    const names = Object.keys(kind === 'structure' ? vocab.structures : vocab.biomes);
    if (!names.length) { toast('vocabulary not loaded yet'); return; }
    const pick = kind === 'structure'
      ? (names.includes('mansion') ? 'mansion' : names[0])
      : (names.includes('jungle') ? 'jungle' : names[0]);
    conds.push({ id: kind.slice(0,1) + (nextId++), kind, name: pick,
                 within: 400, of: 'origin', precision: 'fine' });
    render();
  }
  $('#addStruct').onclick = () => addCond('structure');
  $('#addBiome').onclick  = () => addCond('biome');
  $('#addLoot').onclick = () => {
    const ls = Object.keys(lootVocab.structures || {});
    if (!ls.length) { toast('loot vocabulary not loaded'); return; }
    const st = ls.includes('desert_pyramid') ? 'desert_pyramid' : ls[0];
    const items = lootVocab.structures[st] || [];
    conds.push({ id: 'c' + (nextId++), kind: 'loot', struct: st,
                 item: items.includes('diamond') ? 'diamond' : items[0],
                 count: 1, within: 3000 });
    render();
  };

  $('#addEyes').onclick = () => {
    if (conds.some(c => c.kind === 'eyes')) { toast('one eye condition is enough'); return; }
    conds.push({ id: 'portal', kind: 'eyes', eyes: 6 });
    render();
  };

  $('#addOre').onclick = () => {
    conds.push({ id: 'o' + (nextId++), kind: 'ore', mat: 'diamond',
                 count: ORE_DEFAULT.diamond, within: 64, of: 'origin' });
    render();
  };

  $('#addSlime').onclick = () => {
    conds.push({ id: 's' + (nextId++), kind: 'slime', count: 6, within: 128, of: 'origin' });
    render();
  };

  $('#addArea').onclick = () => {
    const names = Object.keys(vocab.biomes || {});
    if (!names.length) { toast('vocabulary not loaded yet'); return; }
    const pick = names.includes('mushroom_fields') ? 'mushroom_fields'
               : names.includes('badlands') ? 'badlands' : names[0];
    conds.push({ id: 'a' + (nextId++), kind: 'area', name: pick, pct: 50,
                 within: 800, of: 'origin', precision: 'fine' });
    render();
  };

  $('#addIsland').onclick = () => {
    if (conds.some(c => c.kind === 'island')) { toast('one island condition is enough'); return; }
    conds.push({ id: 'i' + (nextId++), kind: 'island', pct: 75, within: 256, of: 'spawn' });
    render();
  };

  $('#addHeight').onclick = () => {
    conds.push({ id: 'h' + (nextId++), kind: 'height', height: 150, relief: 0,
                 within: 300, of: 'origin', precision: 'fine' });
    render();
  };

  $('#addStronghold').onclick = () => {
    conds.push({ id: 'x' + (nextId++), kind: 'stronghold', depth: 0,
                 land: true, within: 3000, of: 'origin' });
    render();
  };

  $('#addOverlap').onclick = () => {
    const names = Object.keys(vocab.structures || {});
    if (!names.length) { toast('vocabulary not loaded yet'); return; }
    const a = names.includes('village') ? 'village' : names[0];
    const b = names.includes('ruined_portal') ? 'ruined_portal' : names[1] || names[0];
    conds.push({ id: 'x' + (nextId++), kind: 'overlap', a, b, pad: 0,
                 within: 2000, of: 'origin' });
    render();
  };

  // Measured on this machine at ~98 seeds/s (locating a stronghold costs ~10ms).
  const EYE_TIME = {0:'instant',1:'instant',2:'instant',3:'instant',4:'instant',
    5:'~3 s', 6:'~20 s', 7:'~4 min', 8:'~50 min', 9:'~18 hours',
    10:'~22 days', 11:'~3 years', 12:'~320 years'};

  function dimOf(c) {
    return (c.kind === 'structure' ? vocab.structures[c.name] : vocab.biomes[c.name]) || '';
  }

  // Every edit re-renders all the cards, which used to drop keyboard focus to
  // <body> after each keystroke-commit or select change -- the builder was
  // unusable without a mouse. The wrapper remembers which control had focus
  // (card position, then control position inside it) and puts it back, caret
  // included, and gives unlabelled controls an accessible name from their
  // title or the text beside them.
  function render() {
    const box = $('#conds');
    const ae = document.activeElement;
    let keep = null;
    if (ae && box.contains(ae)) {
      const card = ae.closest('.cond');
      const ctrls = card ? [...card.querySelectorAll('input,select,button,textarea')] : [];
      keep = { ci: [...box.children].indexOf(card), fi: ctrls.indexOf(ae),
               caret: (typeof ae.selectionStart === 'number') ? ae.selectionStart : null };
    }
    renderCards();
    box.querySelectorAll('input,select,textarea').forEach(n => {
      if (n.getAttribute('aria-label') || n.labels && n.labels.length) return;
      const card = n.closest('.cond'), cid = card && card.querySelector('.cid');
      const prefix = cid ? cid.textContent + ': ' : '';
      const prev = n.previousElementSibling;
      let name = n.title || (prev && prev.textContent) || '';
      if (!name.trim() && n.tagName === 'SELECT' && n.querySelector('option[value="origin"]'))
        name = 'measured from';
      if (!name.trim() && card)
        name = (card.classList[1] || 'condition') + (n.tagName === 'SELECT' ? ' type' : ' value');
      n.setAttribute('aria-label', prefix + name.trim());
    });
    box.querySelectorAll('button.x').forEach(b => {
      const card = b.closest('.cond'), id = card && card.querySelector('.cid');
      if (!b.getAttribute('aria-label')) b.setAttribute('aria-label', 'remove condition ' + (id ? id.textContent : ''));
    });
    if (keep && keep.ci >= 0) {
      const card = box.children[keep.ci];
      const ctrls = card ? [...card.querySelectorAll('input,select,button,textarea')] : [];
      const t = ctrls[keep.fi] || ctrls[0];
      if (t) {
        t.focus();
        try { if (keep.caret !== null && t.setSelectionRange) t.setSelectionRange(keep.caret, keep.caret); } catch (_) {}
      }
    }
  }
  function renderCards() {
    const box = $('#conds'); box.innerHTML = '';
    // The leaderboard offers only conditions that measure something, so it has
    // to be re-derived whenever the condition list changes.
    setTimeout(renderRank, 0);
    if (!conds.length) {
      box.append(el('div', 'empty', 'No conditions yet — add a structure or biome.'));
      return;
    }
    conds.forEach((c, i) => {
      const d = el('div', 'cond ' + c.kind);
      if (c.kind === 'loot') {
        const top = el('div','top');
        top.append(mkIcon('item', c.item), el('span','cid', c.id), el('span','dim','CHEST LOOT'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const ss = el('select');
        Object.keys(lootVocab.structures).forEach(n => {
          const o = el('option', null, pretty(n)); o.value = n;
          if (n === c.struct) o.selected = true; ss.append(o);
        });
        ss.onchange = e => {
          c.struct = e.target.value;
          const items = lootVocab.structures[c.struct] || [];
          if (!items.includes(c.item)) c.item = items[0];
          render();
        };
        const w = el('input'); w.type='number'; w.value=c.within; w.min=100; w.max=10000; w.step=500;
        w.title='search radius (blocks)';
        w.onchange = e => c.within = Math.max(100, Math.min(10000, +e.target.value||3000));
        g.append(ss, w); d.append(g);

        const g2 = el('div','grid');
        const it = el('select');
        (lootVocab.structures[c.struct] || []).sort().forEach(n => {
          const o = el('option', null, pretty(n)); o.value = n;
          if (n === c.item) o.selected = true; it.append(o);
        });
        it.onchange = e => { c.item = e.target.value; render(); };
        const cnt = el('input'); cnt.type='number'; cnt.value=c.count; cnt.min=1; cnt.title='minimum count';
        cnt.onchange = e => c.count = Math.max(1, +e.target.value||1);
        g2.append(it, cnt); d.append(g2);
        // Bastions are the one structure whose chests are only partly modelled
        // (the engine simulates the starting piece, which always generates).
        // Every hit is real; a miss is not proof of absence. Say so where the
        // choice is made, not only in the README.
        if (c.struct === 'bastion')
          d.append(el('div','note warn',
            'bastion loot counts only the chests that always generate — hits are real, but a bastion whose item is in a later chest is missed'));
        box.append(d);
        return;
      }
      if (c.kind === 'ore') {
        const top = el('div','top');
        top.append(mkIcon('item', c.mat), el('span','cid', c.id), el('span','dim','ORE DENSITY'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const ms = el('select');
        ORE_MATERIALS.forEach(n => {
          const o = el('option', null, pretty(n)); o.value = n;
          if (n === c.mat) o.selected = true; ms.append(o);
        });
        ms.onchange = e => { c.mat = e.target.value; c.count = ORE_DEFAULT[c.mat] || 1; render(); };
        const w = el('input'); w.type='number'; w.value=c.within; w.min=16; w.max=256; w.step=16;
        w.title='radius in blocks (all depths)';
        w.onchange = e => c.within = Math.max(16, Math.min(256, +e.target.value||64));
        g.append(ms, w); d.append(g);

        const g2 = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        lab.textContent = 'at least (blocks)';
        const cnt = el('input'); cnt.type='number'; cnt.value=c.count; cnt.min=1;
        cnt.title='minimum ore blocks in range';
        cnt.onchange = e => c.count = Math.max(1, +e.target.value||1);
        g2.append(lab, cnt); d.append(g2);

        const g3 = el('div','grid');
        const vl = el('div');
        vl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        vl.textContent = 'one vein ≥ (0=off)';
        const vn = el('input'); vn.type='number'; vn.value=c.vein||0; vn.min=0; vn.max=64;
        vn.title='minimum blocks in a SINGLE connected vein — the blob you mine in one go, far rarer than the scattered total';
        vn.onchange = e => { c.vein = Math.max(0, Math.min(64, +e.target.value||0)); render(); };
        g3.append(vl, vn); d.append(g3);

        // Deepslate is not a separate ore -- it is the same ore wearing
        // whatever stone it replaced, so "deepslate emerald" is just emerald
        // below y=0. A Y band gets every material the distinction at once.
        const dp = el('label','chk');
        const dpc = el('input'); dpc.type='checkbox'; dpc.checked = !!c.deepslate;
        dpc.onchange = e => { c.deepslate = e.target.checked; render(); };
        dp.append(dpc, document.createTextNode(' deepslate only (below y=0, 1.18+)'));
        d.append(dp);

        const xp = el('label','chk');
        const xpc = el('input'); xpc.type='checkbox'; xpc.checked = !!c.exposed;
        xpc.onchange = e => { c.exposed = e.target.checked; render(); };
        xp.append(xpc, document.createTextNode(' exposed only (open to a cave, 1.18+, SLOW)'));
        d.append(xp);

        const note = el('div','note' + (c.exposed ? ' warn' : ''));
        note.textContent = c.exposed
          ? '⚠ Exposed counts only ore with a non-solid neighbour in Minecraft’s real block terrain — ore you could actually see in a cave wall. Generates terrain per candidate chunk, so it is the slowest filter here: radius is capped at 48 and you should pair it with something cheap first.'
          : (c.vein||0) > 0
          ? 'Vein size is the largest face-connected blob of this ore in the radius. Two placements that happen to touch merge into one bigger vein — which is where records come from.'
          : 'Counts every ore block at all depths in the radius — a richness proxy, not what you’d mine. Expensive; pair with a structure to filter first.';
        d.append(note);
        box.append(d);
        return;
      }
      if (c.kind === 'stronghold') {
        const top = el('div','top');
        top.append(el('span','ic','🏛'), el('span','cid', c.id), el('span','dim','EXPOSED STRONGHOLD'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const dl = el('div');
        dl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        dl.textContent = 'ground over the roof (blocks)';
        const dp = el('input'); dp.type='number'; dp.value = c.depth; dp.min=0; dp.max=32; dp.step=1;
        dp.title = '0 = the stronghold breaks the surface. 4 = a few blocks of dirt over it.';
        dp.onchange = e => { c.depth = Math.max(0, Math.min(32, +e.target.value||0)); render(); };
        g.append(dl, dp); d.append(g);

        const g2 = el('div','grid');
        const wl = el('div');
        wl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        wl.textContent = 'search within (blocks)';
        const w = el('input'); w.type='number'; w.value=c.within; w.min=64; w.max=30000; w.step=500;
        w.onchange = e => c.within = Math.max(64, Math.min(30000, +e.target.value||3000));
        g2.append(wl, w); d.append(g2);

        const lb = el('label'); lb.style.cssText='display:flex;gap:8px;align-items:center;font-size:12.5px';
        const cb = el('input'); cb.type='checkbox'; cb.checked = c.land !== false;
        cb.onchange = e => { c.land = e.target.checked; render(); };
        lb.append(cb, document.createTextNode('on dry land (uncheck to allow the sea floor)'));
        d.append(lb);

        // Read the picker, not a `version` binding -- there isn't one, and a
        // ReferenceError in here takes the whole condition list down with it.
        const ver = ($('#version') && $('#version').value) || '1.21';
        const old = /^1\.(1[0-7]|[0-9])(\D|$)/.test(ver);
        const note = el('div','note' + (old ? '' : ' warn'));
        note.textContent = old
          ? 'A stronghold whose roof reaches the surface. Depth is the roof against the ground above it — a bare Y is meaningless, since y=60 is a metre down on a plain and buried inside a mountain. Heights above y=52 came out right about 2 times in 3 when checked against a real server, and an exposed one is always up there, so treat a hit as a candidate to verify rather than a certainty. Most hits are on an ocean floor; "dry land" is on by default for that reason.'
          : `⚠ ${ver}: from 1.18 a stronghold's roof can never rise above y=52 while the ground sits near y=63, so an exposed one does not exist in this version. These are a pre-1.18 phenomenon.`;
        d.append(note);
        if (!old) {
          // The search would be refused anyway, so make the fix one click rather
          // than an instruction the reader has to carry back to another control.
          const fix = el('button','tiny','switch to 1.17');
          fix.onclick = () => {
            const sel = $('#version');
            if (sel) { sel.value = '1.17'; sel.dispatchEvent(new Event('change')); }
          };
          d.append(fix);
        }
        box.append(d);
        return;
      }
      if (c.kind === 'overlap') {
        const top = el('div','top');
        top.append(el('span','ic','💥'), el('span','cid', c.id), el('span','dim','STRUCTURE OVERLAP'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        // "stronghold" is offered HERE ONLY. It has no region position, so it
        // cannot be a plain structure condition, and the vocabulary the rest of
        // the builder reads deliberately leaves it out. Overlap is the one place
        // the engine can answer for it.
        const names = Object.keys(vocab.structures || {}).concat(['stronghold']);
        const mk = (val, set) => {
          const s = el('select');
          names.forEach(n => { const o = el('option', null, pretty(n)); o.value = n;
            if (n === val) o.selected = true; s.append(o); });
          s.onchange = e => { set(e.target.value); render(); };
          return s;
        };
        const g = el('div','grid');
        g.append(mk(c.a, v => c.a = v), mk(c.b, v => c.b = v));
        d.append(g);

        const g2 = el('div','grid');
        const wl = el('div');
        wl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        wl.textContent = 'search within (blocks)';
        const w = el('input'); w.type='number'; w.value=c.within; w.min=64; w.max=30000; w.step=500;
        w.title='how far from the reference the PAIR may be — not how close the two structures are';
        w.onchange = e => c.within = Math.max(64, Math.min(30000, +e.target.value||2000));
        g2.append(wl, w); d.append(g2);

        const g3 = el('div','grid');
        const pl = el('div');
        pl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        pl.textContent = 'slack (blocks)';
        const pd = el('input'); pd.type='number'; pd.value=c.pad||0; pd.min=0; pd.max=128; pd.step=8;
        pd.title='0 = the footprints must actually meet; higher = "practically touching"';
        pd.onchange = e => c.pad = Math.max(0, Math.min(128, +e.target.value||0));
        g3.append(pl, pd); d.append(g3);

        const isSh = (c.a === 'stronghold' || c.b === 'stronghold');
        if (isSh) {
          const g4 = el('div','grid');
          const rl = el('div');
          rl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
          rl.textContent = 'stronghold roof at least y';
          const rf = el('input'); rf.type='number'; rf.value = (c.minRoofY ?? '');
          rf.min=-64; rf.max=200; rf.step=1; rf.placeholder='any';
          rf.title='Blank = a stronghold anywhere under the partner. 55-62 = the '
                 + 'part that meets it reaches the surface, which is the rare "exposed" case.';
          rf.onchange = e => {
            const v = e.target.value.trim();
            c.minRoofY = (v === '') ? null : Math.max(-64, Math.min(200, +v || 0));
            render();
          };
          g4.append(rl, rf); d.append(g4);

          // Ground over the piece that meets the partner. This is the question
          // "is it EXPOSED"; the roof height above is only a proxy for it, and
          // the two disagree in exactly the case that matters -- a roof at y=60
          // under a hill is five blocks down, not out.
          const g5 = el('div','grid');
          const dl = el('div');
          dl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
          dl.textContent = 'ground above it, at most';
          const df = el('input'); df.type='number'; df.value = (c.maxDepth ?? '');
          df.min=0; df.max=64; df.step=1; df.placeholder='any';
          df.title='0 = the stronghold BREAKS THE SURFACE where it meets the partner '
                 + '(the real "exposed"). 4 = a few blocks of dirt over it. Blank = do not '
                 + 'test the ground at all, only the roof height above.';
          df.onchange = e => {
            const v = e.target.value.trim();
            c.maxDepth = (v === '') ? null : Math.max(0, Math.min(64, +v || 0));
            render();
          };
          g5.append(dl, df); d.append(g5);
        }

        const dimA = vocab.structures[c.a] || '', dimB = vocab.structures[c.b] || '';
        const note = el('div','note' + (dimA && dimB && dimA !== dimB ? ' warn' : ''));
        note.textContent = (dimA && dimB && dimA !== dimB)
          ? `⚠ ${pretty(c.a)} is in the ${dimA} and ${pretty(c.b)} is in the ${dimB} — they can never overlap.`
          : isSh
          ? 'A stronghold is tested against its REAL piece boxes, not a nominal square — it sprawls 112 blocks in every direction, so a square that size would match almost anything nearby. For "exposed" use “ground above it, at most” = 0: that measures the terrain over the piece that meets the partner. The roof height is only a Y number with no ground in it — asking for roof ≥ 55 returns strongholds under 7–13 blocks of dirt, which is not the find you meant. Leave both blank for "a stronghold somewhere underneath". This walks the stronghold rings with the biome generator (~1 ms per seed), so it is far slower than any other condition here.'
          : 'Two structures generating into the same ground: a ruined portal inside a village, a shipwreck through a monument. Footprints are NOMINAL boxes (a village’s reach is approximated), so a hit is a strong candidate worth loading, not a proof.';
        d.append(note);

        // A stronghold ROOF HEIGHT is version-locked in exactly the way the
        // standalone condition is, and this panel used to say nothing about it
        // -- so "exposed stronghold inside a village", built here on the
        // default version, was refused outright by the engine with the reason
        // living only in an error the builder never showed. Measured over 180
        // strongholds at 26.2: the roof tops out at y=52 (1 in 180 reaches it)
        // while the ground sits near y=63, so nothing is ever exposed.
        if (isSh && c.minRoofY != null) {
          const shVer = ($('#version') && $('#version').value) || '1.21';
          const shOld = /^1\.(1[0-7]|[0-9])(\D|$)/.test(shVer);
          if (!shOld && c.minRoofY > 52) {
            const w = el('div','note bad',
              `⚠ ${shVer}: a stronghold roof can never rise above y=52 from 1.18 on, so `
              + `roof ≥ ${c.minRoofY} has no solutions and the engine will refuse this `
              + `query outright. Exposed strongholds are a pre-1.18 phenomenon.`);
            d.append(w);
            const fix = el('button','tiny','switch to 1.17');
            fix.onclick = () => {
              const sel = $('#version');
              if (sel) { sel.value = '1.17'; sel.dispatchEvent(new Event('change')); }
            };
            d.append(fix);
          } else if (!shOld && c.minRoofY >= 45) {
            d.append(el('div','note warn',
              `⚠ ${shVer}: y=52 is the ceiling from 1.18 on, and only about 1 stronghold `
              + `in 180 reaches it — so roof ≥ ${c.minRoofY} is the extreme tail and will `
              + `search for a very long time. It is also still ~11 blocks underground, `
              + `not exposed. Use 1.17 for a stronghold that actually breaks the surface.`));
          }
        }
        box.append(d);
        return;
      }
      if (c.kind === 'slime') {
        const top = el('div','top');
        top.append(mkIcon('slime'), el('span','cid', c.id), el('span','dim','SLIME CHUNKS'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        lab.textContent = 'at least (chunks)';
        const cnt = el('input'); cnt.type='number'; cnt.value=c.count; cnt.min=1;
        cnt.title='minimum slime chunks in range';
        cnt.onchange = e => c.count = Math.max(1, +e.target.value||1);
        g.append(lab, cnt); d.append(g);

        const g2 = el('div','grid');
        const wl = el('div');
        wl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        wl.textContent = 'within (blocks)';
        const w = el('input'); w.type='number'; w.value=c.within; w.min=16; w.max=2048; w.step=16;
        w.title='radius in blocks';
        w.onchange = e => c.within = Math.max(16, Math.min(2048, +e.target.value||128));
        g2.append(wl, w); d.append(g2);

        const note = el('div','note');
        note.textContent = 'Slime chunks are a per-chunk RNG check on the world seed — version-independent, so this works for any MC version. A radius-128 disc holds ~200 chunks (~20 slime on average); ask for a dense cluster for a slime-farm site.';
        d.append(note);
        box.append(d);
        return;
      }
      if (c.kind === 'island') {
        const top = el('div','top');
        top.append(el('span','ic','🏝️'), el('span','cid', c.id), el('span','dim','ISLAND SPAWN'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const wl = el('div');
        wl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        wl.textContent = 'within (blocks)';
        const w = el('input'); w.type='number'; w.value=c.within; w.min=48; w.max=2048; w.step=64;
        w.title='radius of open water around the island';
        w.onchange = e => c.within = Math.max(48, Math.min(2048, +e.target.value||256));
        g.append(wl, w); d.append(g);

        const g2 = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        lab.textContent = 'ocean at least (%)';
        const pc = el('input'); pc.type='number'; pc.value=c.pct; pc.min=1; pc.max=100;
        pc.title='minimum % of the disc that is ocean';
        pc.onchange = e => c.pct = Math.max(1, Math.min(100, +e.target.value||75));
        g2.append(lab, pc); d.append(g2);

        const g3 = el('div','grid');
        const of = el('select');
        ['spawn','origin'].forEach(o => { const n = el('option', null,
          o === 'spawn' ? 'from world spawn' : 'from origin (0,0)'); n.value = o;
          if (o === c.of) n.selected = true; of.append(n); });
        of.onchange = e => { c.of = e.target.value; render(); };
        g3.append(of); d.append(g3);

        const note = el('div','note');
        note.textContent = 'Spawn on a small island: the reference point is land, and ocean covers at least the given share of the surrounding disc. Higher % + bigger radius = a smaller, more isolated island (survival-island start).';
        d.append(note);
        box.append(d);
        return;
      }
      if (c.kind === 'area') {
        const top = el('div','top');
        top.append(mkIcon('biome', c.name), el('span','cid', c.id), el('span','dim','BIOME AREA'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const sel = el('select');
        Object.keys(vocab.biomes).sort().forEach(nm => {
          const o = el('option', null, pretty(nm)); o.value = nm;
          if (nm === c.name) o.selected = true; sel.append(o);
        });
        sel.onchange = e => { c.name = e.target.value; render(); };
        const w = el('input'); w.type='number'; w.value=c.within; w.min=64; w.max=4000; w.step=100;
        w.title='radius in blocks';
        w.onchange = e => c.within = Math.max(64, Math.min(4000, +e.target.value||800));
        g.append(sel, w); d.append(g);

        const g2 = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        lab.textContent = 'covers at least (%)';
        const pc = el('input'); pc.type='number'; pc.value=c.pct; pc.min=1; pc.max=100;
        pc.title='minimum % of the disc that is this biome';
        pc.onchange = e => c.pct = Math.max(1, Math.min(100, +e.target.value||50));
        g2.append(lab, pc); d.append(g2);

        const note = el('div','note');
        note.textContent = 'Keeps seeds where this biome fills at least the given share of the disc — how you find a genuinely large biome (a huge mushroom island, a sprawling mesa). Big radius + high % = a big biome. Sampled at the chosen precision, so a coarse scan only ever misses seeds, never invents them.';
        d.append(note);
        box.append(d);
        return;
      }
      if (c.kind === 'height') {
        const top = el('div','top');
        top.append(mkIcon('height'), el('span','cid', c.id), el('span','dim','TERRAIN HEIGHT'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);

        const g = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        lab.textContent = 'peak at least (Y)';
        const hy = el('input'); hy.type='number'; hy.value=c.height; hy.min=-64; hy.max=320;
        hy.title='minimum approximate surface height in the disc';
        hy.onchange = e => c.height = Math.max(-64, Math.min(320, +e.target.value||150));
        g.append(lab, hy); d.append(g);

        const gr = el('div','grid');
        const rl = el('div');
        rl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        rl.textContent = 'relief ≥ (drop, 0=off)';
        const rv = el('input'); rv.type='number'; rv.value=c.relief||0; rv.min=0; rv.max=320; rv.step=10;
        rv.title='minimum peak-to-valley height spread in the disc — big = steep terrain (a structure on a cliff edge / clinging to a mountainside)';
        rv.onchange = e => { c.relief = Math.max(0, Math.min(320, +e.target.value||0)); render(); };
        gr.append(rl, rv); d.append(gr);

        const g2 = el('div','grid');
        const wl = el('div');
        wl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        wl.textContent = 'within (blocks)';
        const w = el('input'); w.type='number'; w.value=c.within; w.min=4; w.max=2048; w.step=32;
        w.title='radius in blocks (use a small radius + of: a structure for "tall structure")';
        w.onchange = e => c.within = Math.max(4, Math.min(2048, +e.target.value||300));
        g2.append(wl, w); d.append(g2);

        const ex = el('label','chk');
        const exc = el('input'); exc.type='checkbox'; exc.checked = !!c.exact;
        exc.onchange = e => { c.exact = e.target.checked; render(); };
        ex.append(exc, document.createTextNode(' exact terrain (real block-level, 1.18+, SLOW)'));
        d.append(ex);

        const note = el('div','note' + (c.exact ? '' : ' warn'));
        note.textContent = c.exact
          ? '✓ Exact: samples Minecraft’s real block-level terrain (not the smoothed estimate), so it sees sharp cliffs right under a structure’s footprint. Use a SMALL radius (~8-12 = the footprint) with of: a structure. Costly per seed — the planner runs it last, so always pair it with a cheap filter first (e.g. an approx terrain-height ≥150 at the same structure). 1.18+ Overworld only.'
          : (c.relief||0) > 0
          ? '⚠ Approximate (smoothed 1:4 estimate). Good as a cheap pre-filter, but it CANNOT see sharp cliffs — for a real cliff-edge result tick “exact terrain” below and use a small radius. Overworld only; best on 1.18+.'
          : '⚠ Approximate. Terrain height is cubiomes’ estimate (as used for spawn finding), not exact game height — it can be off by a few blocks, so leave headroom. Overworld only; most accurate on 1.18+. For clifftop / cliff-edge placement use “relief” + “exact terrain”.';
        d.append(note);
        box.append(d);
        return;
      }
      if (c.kind === 'eyes') {
        const top = el('div','top');
        top.append(mkIcon('eyes'), el('span','cid', c.id), el('span','dim','END PORTAL'));
        const x = el('button','tiny x','×');
        x.onclick = () => { conds.splice(i,1); render(); };
        top.append(x); d.append(top);
        const g = el('div','grid');
        const lab = el('div');
        lab.style.cssText = 'font-size:12.5px;align-self:center';
        lab.textContent = 'at least';
        const n = el('input'); n.type='number'; n.min=0; n.max=12; n.value=c.eyes;
        n.onchange = e => { c.eyes = Math.max(0, Math.min(12, +e.target.value||0)); render(); };
        g.append(lab, n); d.append(g);
        const est = el('div','note' + (c.eyes >= 9 ? ' warn' : ''));
        est.textContent = `${c.eyes} of 12 frames — expect ${EYE_TIME[c.eyes] || '?'}`
          + (c.eyes >= 11 ? '. This is a distributed-compute target, not a single machine one.' : '');
        d.append(est);
        box.append(d);
        return;
      }

      const top = el('div', 'top');
      top.append(mkIcon(c.kind, c.name), el('span', 'cid', c.id));
      const dim = dimOf(c);
      if (dim) top.append(el('span', 'dim', dim));
      const x = el('button', 'tiny x', '×');
      x.title = 'remove'; x.onclick = () => { conds.splice(i,1); render(); };
      top.append(x);
      d.append(top);

      const g = el('div', 'grid');
      const sel = el('select');
      const names = Object.keys(c.kind === 'structure' ? vocab.structures : vocab.biomes);
      names.sort().forEach(n => {
        const o = el('option', null, pretty(n)); o.value = n;
        if (n === c.name) o.selected = true; sel.append(o);
      });
      sel.onchange = e => { c.name = e.target.value; render(); };

      const w = el('input'); w.type = 'number'; w.value = c.within;
      w.min = 1; w.step = 50; w.title = 'radius in blocks';
      w.onchange = e => c.within = Math.max(1, +e.target.value || 1);
      g.append(sel, w); d.append(g);

      const g2 = el('div', 'grid' + (c.kind === 'biome' ? '' : ' full'));
      const of = el('select');
      // A condition that says something is NOT there has no position, so it
      // cannot be measured from.
      const opts = ['origin', 'spawn', ...conds.filter(o => o.id !== c.id && !o.not).map(o => o.id)];
      opts.forEach(o => {
        const label = o === 'origin' ? 'from origin (0,0)'
                    : o === 'spawn'  ? 'from world spawn (slower)'
                    : 'from ' + o;
        const n = el('option', null, label);
        n.value = o; if (o === c.of) n.selected = true; of.append(n);
      });
      of.onchange = e => { c.of = e.target.value; render(); };
      g2.append(of);

      if (c.kind === 'biome') {
        const p = el('select'); p.title = 'scan precision';
        [['fast','fast'],['fine','fine'],['exact','exact']].forEach(([v,t]) => {
          const o = el('option', null, t); o.value = v;
          if (v === c.precision) o.selected = true; p.append(o);
        });
        p.onchange = e => c.precision = e.target.value;
        g2.append(p);
      }
      d.append(g2);

      // "not": there must be NONE of this in range -- "no ocean within 300".
      const nrow = el('label', 'chk');
      const ncb = el('input'); ncb.type = 'checkbox'; ncb.checked = !!c.not;
      ncb.onchange = e => { c.not = e.target.checked; render(); };
      nrow.append(ncb, document.createTextNode(' none within this radius (must NOT be here)'));
      d.append(nrow);
      if (c.not) d.append(el('div', 'note', c.kind === 'biome'
        ? 'Absence is checked on every 4-block cell in the radius, so a small patch cannot slip between samples. Slower than a presence check; nothing can be measured from it.'
        : 'Every placement attempt in range must fail to generate in the real game (biome and terrain rules included). Nothing can be measured from it.'));

      // Structure cluster count: >= N of this structure within the radius.
      if (c.kind === 'structure' && !c.not) {
        const g3 = el('div','grid');
        const cl = el('div');
        cl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        cl.textContent = 'at least (count)';
        const cn = el('input'); cn.type='number'; cn.value=c.count||1; cn.min=1; cn.max=12;
        cn.title='minimum instances (1 = a normal single match)';
        cn.onchange = e => { c.count = Math.max(1, Math.min(12, +e.target.value||1)); render(); };
        g3.append(cl, cn); d.append(g3);
        if ((c.count||1) > 1) {
          const g4 = el('div','grid');
          const sl = el('div');
          sl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
          sl.textContent = 'tight within (0 = off)';
          const sp = el('input'); sp.type='number'; sp.value=c.spread||0; sp.min=0; sp.max=2048; sp.step=16;
          sp.title='0 = count within the radius of the reference; >0 = the instances must fit within this many blocks of each other, anywhere in the radius (quad-hut style)';
          sp.onchange = e => { c.spread = Math.max(0, Math.min(2048, +e.target.value||0)); render(); };
          g4.append(sl, sp); d.append(g4);
          const note = el('div','note');
          note.textContent = (c.spread||0) > 0
            ? `Tight cluster: finds ${c.count} of this structure packed within ${c.spread} blocks of each other, anywhere within the search radius (the “quad huts” search). Rare — expect a long scan. Variant filters disabled.`
            : `Cluster: finds ${c.count} of this structure within the radius of the reference (e.g. a triple village near spawn). Set “tight within” for a packed-together cluster anywhere. Variant filters disabled.`;
          d.append(note);
        }
      }

      // Variant filters, each offered only on the structure that has it.
      if (c.kind === 'structure' && (c.count || 1) === 1 && !c.not) {
        VARIANTS.filter(v => v.applies(c.name)).forEach(v => {
          const row = el('label','chk');
          const cb = el('input'); cb.type = 'checkbox'; cb.checked = !!c[v.key];
          cb.onchange = e => { c[v.key] = e.target.checked; };
          row.append(cb, document.createTextNode(' ' + v.label));
          d.append(row);
        });
      }
      // Geode shape: size is 3 or 4, and the crack is a 95% draw -- so the
      // interesting ask is the SEALED one, not the cracked one.
      if (c.kind === 'structure' && c.name === 'geode') {
        const g = el('div','grid');
        const gl = el('div');
        gl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        gl.textContent = 'size';
        const gs = el('select');
        [['any',0],['4 (big)',4],['3+',3]].forEach(([lbl,v]) => {
          const o = el('option', null, lbl); o.value = String(v);
          if ((c.gsize||0) === v) o.selected = true; gs.append(o);
        });
        gs.onchange = e => { c.gsize = +e.target.value; render(); };
        g.append(gl, gs); d.append(g);

        const g2 = el('div','grid');
        const cl = el('div');
        cl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
        cl.textContent = 'crack';
        const cs = el('select');
        [['any',''],['sealed (1 in 20)','sealed'],['cracked open','cracked']].forEach(([lbl,v]) => {
          const o = el('option', null, lbl); o.value = v;
          if ((c.sealed||'') === v) o.selected = true; cs.append(o);
        });
        cs.onchange = e => { c.sealed = e.target.value; render(); };
        g2.append(cl, cs); d.append(g2);

        d.append(el('div','note',
          'Amethyst geodes are placed from the chunk population seed, so the finder locates them with the full world seed in pass 2 (nothing can be measured FROM one). Size is 3 or 4; 19 in 20 geodes are cracked open, so “sealed” is the rare, intact one.'));
      }
      box.append(d);
    });
  }

  const isRuinedPortal = n => n === 'ruined_portal' || n === 'ruined_portal_nether';

  // Structure variant filters (getVariant flags), each shown only where it applies.
  const VARIANTS = [
    { key:'surface',   label:'surface only (skip buried portals)', applies: isRuinedPortal },
    { key:'giant',     label:'giant portal only',                  applies: isRuinedPortal },
    { key:'abandoned', label:'zombie village only',                applies: n => n === 'village' },
    { key:'basement',  label:'basement only',                      applies: n => n === 'igloo' },
    { key:'ship',      label:'has an end ship (elytra)',           applies: n => n === 'end_city' },
  ];

  // Mirrors the material table in src/ore.c. Overworld first, then nether.
  const ORE_MATERIALS = ['diamond','iron','gold','emerald','redstone','lapis',
    'copper','coal','quartz','ancient_debris','nether_gold'];
  // A sensible starting threshold per material at the default 64-block radius,
  // so the count field isn't a mystery number. These are rough medians.
  const ORE_DEFAULT = {diamond:1400, iron:4000, gold:1200, emerald:20,
    redstone:1900, lapis:1300, copper:4500, coal:5500, quartz:11000,
    ancient_debris:90, nether_gold:3800};

  // ---- build the query ---------------------------------------------------
  function buildQuery() {
    // Untouched since it was loaded (from a prompt, a share link or a re-run):
    // run EXACTLY that query, every field the builder cannot show included.
    if (loadedQuery && builderSnapshot() === loadedSnap)
      return JSON.parse(JSON.stringify(loadedQuery));
    loadedQuery = null;
    const q = buildConditions();
    if (rank.on && rankable().some(c => c.id === rank.of))
      q.rank = { of: rank.of, by: rank.by || 'auto', top: rank.top || 10 };
    return q;
  }

  function buildConditions() {
    const q = {
      version: $('#version').value,
      conditions: conds.map(c => {
        if (c.kind === 'eyes') return { id: c.id, eyes: c.eyes };
        if (c.kind === 'loot') return { id: c.id, within: c.within,
          loot: { structure: c.struct, item: c.item, count: c.count } };
        if (c.kind === 'ore') {
          const o = { id: c.id, ore: c.mat, count: c.count,
                      within: c.within, of: c.of };
          if ((c.vein || 0) > 0) o.vein = c.vein;
          if (c.exposed) o.exposed = true;
          if (c.deepslate) o.deepslate = true;
          return o;
        }
        if (c.kind === 'stronghold') {
          const o = { id: c.id, stronghold: true, max_depth: c.depth,
                      within: c.within, of: c.of };
          if (c.land === false) o.land = false;
          return o;
        }
        if (c.kind === 'overlap') {
          const o = { id: c.id, overlap: [c.a, c.b], within: c.within, of: c.of };
          if ((c.pad || 0) > 0) o.pad = c.pad;
          // null and 0 mean different things: 0 is a real threshold on 1.18+,
          // where a stronghold roof can sit below y=0.
          if (c.minRoofY !== null && c.minRoofY !== undefined) o.min_roof_y = c.minRoofY;
          // Likewise here, and more sharply: max_depth 0 IS the exposed case,
          // so an absent value and a zero must not collapse together.
          if (c.maxDepth !== null && c.maxDepth !== undefined) o.max_depth = c.maxDepth;
          return o;
        }
        if (c.kind === 'slime') return { id: c.id, slime: c.count,
          within: c.within, of: c.of };
        if (c.kind === 'area') return { id: c.id, biome_area: c.name, pct: c.pct,
          within: c.within, of: c.of, precision: c.precision };
        if (c.kind === 'island') return { id: c.id, island: true, pct: c.pct,
          within: c.within, of: c.of };
        if (c.kind === 'height') {
          const h = { id: c.id, within: c.within, of: c.of, precision: c.precision };
          const peak = (c.height == null ? -64 : c.height);
          if ((c.relief || 0) > 0) h.relief = c.relief;
          // Send a peak floor only when it constrains (> world floor), so a pure
          // relief search (steep terrain) isn't also filtered by height.
          if (peak > -64 || h.relief === undefined) h.height = peak;
          if (c.exact) h.exact = true;    // real block-level terrain (slow, 1.18+)
          return h;
        }
        const o = { id: c.id, within: c.within, of: c.of };
        if (c.kind === 'structure' && c.not) {
          o.structure = c.name; o.not = true;               // absence: no count/variant
        }
        else if (c.kind === 'structure') {
          o.structure = c.name;
          if ((c.count || 1) > 1) {                            // cluster
            o.count = c.count;
            if ((c.spread || 0) > 0) o.spread = c.spread;      // tight cluster
          }
          else VARIANTS.forEach(v => { if (c[v.key] && v.applies(c.name)) o[v.key] = true; });
          if (c.name === 'geode') {
            if ((c.gsize || 0) > 0) o.size = c.gsize;
            if (c.sealed === 'sealed') o.cracked = false;
            if (c.sealed === 'cracked') o.cracked = true;
          }
        }
        else { o.biome = c.name; o.precision = c.precision; if (c.not) o.not = true; }
        return o;
      })
    };
    if (largeBiomes()) q.large_biomes = true;
    return q;
  }

  // The world preset is a property of the world, so it rides along with every
  // request that generates one -- search, map and describe alike. A result found
  // in Large Biomes and then drawn with the default generator is a picture of a
  // different world, which is worse than no picture.
  const largeBiomes = () => !!$('#largeBiomes').checked;

  // ---- leaderboard --------------------------------------------------------
  // A ranked search turns the finder from a filter into an optimiser: it scans
  // the whole range and keeps the best N by one condition's measurement. Only
  // conditions that MEASURE something can be ranked -- a plain "is there a
  // village" has no magnitude.
  const rank = { on: false, of: '', by: 'auto', top: 10 };

  function rankable() {
    return conds.filter(c => ['height','ore','slime','area','island','eyes','overlap'].includes(c.kind)
      || (c.kind === 'structure' && ((c.count || 1) > 1 || c.name === 'geode')));
  }

  function rankMetrics(c) {
    if (!c) return ['auto'];
    if (c.kind === 'height') return ['auto','height','relief'];
    if (c.kind === 'ore')    return ['auto','count','vein'];
    if (c.kind === 'structure' && c.name === 'geode') return ['auto','size'];
    if (c.kind === 'area' || c.kind === 'island')     return ['auto','pct'];
    if (c.kind === 'overlap') return ['auto','area'];
    return ['auto','count'];
  }

  function renderRank() {
    const box = $('#rankBox'); if (!box) return;
    box.innerHTML = '';
    const opts = rankable();
    const chk = el('label','chk');
    const cb = el('input'); cb.type='checkbox'; cb.checked = rank.on && opts.length > 0;
    cb.disabled = !opts.length;
    cb.onchange = e => { rank.on = e.target.checked; renderRank(); };
    chk.append(cb, document.createTextNode(' leaderboard: rank the whole range instead of stopping early'));
    box.append(chk);
    if (!opts.length) {
      box.append(el('div','note','Add a condition that measures something — terrain height, ore, slime chunks, biome area, island, eyes, a structure cluster, a geode, or an overlap — and you can rank by it.'));
      return;
    }
    if (!opts.some(c => c.id === rank.of)) rank.of = opts[0].id;
    if (!rank.on) return;
    const cur = opts.find(c => c.id === rank.of);
    const g = el('div','grid');
    const sel = el('select');
    opts.forEach(c => { const o = el('option', null, `${c.id} · ${c.kind}`); o.value = c.id;
      if (c.id === rank.of) o.selected = true; sel.append(o); });
    sel.onchange = e => { rank.of = e.target.value; rank.by = 'auto'; renderRank(); };
    const bysel = el('select');
    rankMetrics(cur).forEach(mname => { const o = el('option', null, mname); o.value = mname;
      if (mname === rank.by) o.selected = true; bysel.append(o); });
    bysel.onchange = e => rank.by = e.target.value;
    g.append(sel, bysel); box.append(g);
    const g2 = el('div','grid');
    const tl = el('div');
    tl.style.cssText = 'font-size:12.5px;align-self:center;color:var(--ink-soft)';
    tl.textContent = 'keep top';
    const t = el('input'); t.type='number'; t.value=rank.top; t.min=1; t.max=50;
    t.onchange = e => rank.top = Math.max(1, Math.min(50, +e.target.value||10));
    g2.append(tl, t); box.append(g2);
    box.append(el('div','note',
      'Ranked runs scan every seed in the range — no early stop — so the range sets the runtime, and "best" means best of exactly that many seeds. Set the ranked condition’s own threshold LOW; a tight threshold leaves the leaderboard empty.'));
  }

  // ---- rendering output --------------------------------------------------
  const out = $('#out');
  // Every action renders into #out, so make sure it can be SEEN. Tool runs and
  // their errors used to land behind the still-open drawer and below the
  // full-height hero -- the same "message never seen" failure the toasts fixed.
  function reveal() {
    closeDrawer();
    $('#main').classList.add('searched');
    out.scrollIntoView({ block: 'start', behavior: 'smooth' });
  }
  const busy = t => {
    reveal();
    out.innerHTML = `<div class="card"><div class="busy">${esc(t)}</div></div>`;
  };
  const fail = (e) => { reveal(); out.innerHTML =
    `<div class="err"><b>error</b>${esc(e)}</div>`; announce('error: ' + e); };

  function nsFmt(n) {
    if (n >= 1e6) return (n/1e6).toFixed(1) + ' ms';
    if (n >= 1e3) return (n/1e3).toFixed(1) + ' µs';
    return Math.round(n) + ' ns';
  }

  function planCard(plan) {
    const c = el('div', 'card');
    c.append(el('h3', null, 'Execution plan — reordered by measured cost'));
    const maxNs = Math.max(1, ...[...plan.pass1, ...plan.pass2].map(s => s.ns));
    [['pass1','pass 1 · 48-bit geometry · no biome generator yet'],
     ['pass2','pass 2 · biome-dependent · only for pass-1 survivors']]
    .forEach(([k, title]) => {
      if (!plan[k].length) return;
      const p = el('div', 'pass');
      const hd = el('div', 'hd');
      hd.append(el('span', null, title));
      p.append(hd);
      plan[k].forEach((s, i) => {
        const row = el('div', 'step');
        row.append(el('span','n', (i+1)+'.'), el('span','d', s.desc));
        const bar = el('div','bar'), fill = el('i');
        fill.style.width = Math.max(2, 100*s.ns/maxNs) + '%';
        if (s.ns > 1e5) fill.className = 'slow';
        bar.append(fill);
        row.append(bar, el('span','c', nsFmt(s.ns)));
        p.append(row);
      });
      c.append(p);
    });
    const t1 = plan.pass1.reduce((a,s)=>a+s.ns, 0);
    const t2 = plan.pass2.reduce((a,s)=>a+s.ns, 0);
    if (t1 > 0 && t2 > 0) {
      const ratio = t2 / t1;
      c.append(el('div','note',
        `Pass 1 totals ${nsFmt(t1)} per seed; pass 2 totals ${nsFmt(t2)} — ` +
        `${ratio >= 10 ? Math.round(ratio).toLocaleString() : ratio.toFixed(1)}× more expensive. ` +
        `The bars share one scale, so pass 1 looks like a sliver: that gap is exactly why it runs first.`));
    }
    if (plan.conditions.length) {
      const written = plan.conditions.map(x => x.id).join(', ');
      const execd = plan.pass1.map(s => {
        const hit = plan.conditions.find(x => s.desc.startsWith(x.desc.split(' ')[0]));
        return hit ? hit.id : null;
      }).filter(Boolean).join(', ');
      if (execd && written !== execd)
        c.append(el('div','note',
          `Written: ${written} — executed: ${execd}. The planner reordered them.`));
    }
    return c;
  }

  function statsCard(title, items) {
    const c = el('div','card'); c.append(el('h3', null, title));
    const s = el('div','stats');
    items.forEach(([k, v, tone]) => {
      const d = el('div','stat');
      d.append(el('div','k', k));
      const val = el('div','v' + (tone ? ' ' + tone : ''), v);
      d.append(val); s.append(d);
    });
    c.append(s); return c;
  }

  function noteCards(notes) {
    return (notes || []).map(n => el('div','note warn', n));
  }

  function rawCard(raw) {
    const d = el('details');
    d.append(el('summary', null, 'raw tool output'));
    const p = el('pre'); p.textContent = raw; d.append(p);
    return d;
  }

  // ---- actions -----------------------------------------------------------
  // `signal` is optional: pass a run's AbortSignal to make the request stoppable.
  async function post(path, body, signal) {
    const r = await fetch(path, { method:'POST', signal,
      headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) });
    // Not every failure is our JSON: a proxy, a crashed handler or a stopped
    // server answers with HTML or nothing, and r.json() then surfaced as
    // "Unexpected token <" -- true, and no help at all.
    const text = await r.text();
    let d;
    try { d = JSON.parse(text); }
    catch (_) {
      throw new Error(r.ok ? 'the server replied with something that is not JSON'
                           : `HTTP ${r.status}${text ? ': ' + text.slice(0, 200) : ''}`);
    }
    if (!r.ok) throw new Error(d.error || ('HTTP ' + r.status));
    return d;
  }

  function guard() {
    if (!conds.length) { fail('Add at least one condition first.'); return false; }
    return true;
  }

  $('#planBtn').onclick = async () => {
    if (!guard()) return;
    closeDrawer(); busy('planning…');
    try {
      const d = await post('/api/plan', { query: buildQuery() });
      out.innerHTML = ''; out.append(...noteCards(d.notes), planCard(d.plan), rawCard(d.raw));
    } catch (e) { fail(e.message); }
  };

  $('#explainBtn').onclick = async () => {
    if (!guard()) return;
    closeDrawer(); busy('sampling the seed space…');
    try {
      const d = await post('/api/explain',
        { query: buildQuery(), threads: +$('#threads').value });
      const e = d.explain;
      out.innerHTML = '';
      const tone = v => v == null ? '' : v > 50 ? 'bad' : v > 5 ? 'warn' : 'good';
      out.append(statsCard('Estimate — will this finish?', [
        ['pass 1 survival', (e.pass1_pct ?? '—') + '%', tone(e.pass1_pct)],
        ['seeds per hit', e.seeds_per_hit != null ? Math.round(e.seeds_per_hit).toLocaleString() : '—'],
        ['predicted rate', e.rate || '—'],
        ['first hit', e.first_hit || '—'],
        ['12 hits', e.twelve || '—'],
      ]));
      if (e.verdict) {
        const bad = /too rare|impossible|not/i.test(e.verdict);
        out.append(el('div','note' + (bad ? ' bad' : ''), 'Verdict: ' + e.verdict));
      }
      if (e.pass1_pct > 50) out.append(el('div','note warn',
        `Pass 1 keeps ${e.pass1_pct}% of seeds — it isn't filtering, so every seed pays full biome cost. Tighten a radius.`));
      if (e.pass1_share != null) out.append(el('div','note',
        `Time split: pass 1 ${e.pass1_share}%, pass 2 ${(100-e.pass1_share).toFixed(1)}%.`));
      out.append(...noteCards(d.notes), planCard(d.plan), rawCard(d.raw));
    } catch (e) { fail(e.message); }
  };

  // ---- streaming search + live seed scroll -------------------------------
  function setBusyUI(on) {
    $('#go').disabled = on;
    // Plan and Estimate render into the same results area; letting them run
    // mid-search put a plan on top of a live result list.
    ['#searchBtn', '#planBtn', '#explainBtn'].forEach(id => { const b = $(id); if (b) b.disabled = on; });
  }
  function startScroll() {
    const s = $('#scroll'); s.classList.remove('hidden');
    $('#scrollTrack').innerHTML = ''; $('#scrollMeta').textContent = 'starting…';
  }
  function stopScroll() { $('#scroll').classList.add('hidden'); }
  let scanRange = 0;          // seeds requested this run, for the progress line
  let runStart = 0;           // when this run started (ms)
  let runBudget = 0;          // seconds find was given (the server says, first line)
  let runBudgetDefault = false; // true = no budget was set; the server's own cap
  let liveSeeds = [];         // hits streamed so far -- kept when a run is stopped
  let liveCard = null;
  // The query behind the results currently on screen. renderResults needs it to
  // decide whether to show the world-spawn caveat, and it is NOT a parameter
  // there -- reaching for `query` inside renderResults threw on every search.
  let shownQuery = null;
  // The last finished result. "Back" from a map or a describe used to press
  // Search again -- re-running a long or ranked search from scratch just to
  // see results that were already on screen a moment ago.
  let lastResult = null;

  const fmtN = n => { n = Number(n) || 0;
    return n >= 1e9 ? (n / 1e9).toFixed(1) + 'B'
         : n >= 1e6 ? (n / 1e6).toFixed(n >= 1e7 ? 0 : 1) + 'M'
         : n >= 1e3 ? (n / 1e3).toFixed(n >= 1e4 ? 0 : 1) + 'k' : String(Math.round(n)); };
  const fmtT = s => { s = Math.max(0, Math.round(s));
    return s < 60 ? s + ' s'
         : s < 3600 ? Math.floor(s / 60) + ' min ' + String(s % 60).padStart(2, '0') + ' s'
         : Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' min'; };

  function pushSeed(seed, hit, scanned) {
    const t = $('#scrollTrack');
    const d = el('div', 's' + (hit ? ' hit' : ''), (hit ? '✓ found  ' : 'testing  ') + seed);
    t.appendChild(d);                                  // newest at the bottom
    while (t.childElementCount > 20) t.removeChild(t.firstChild);
    if (!hit && scanned !== undefined) progressLine(Number(scanned));
  }
  // One line that answers "how is it going": how far, how fast, how long is
  // left, how many found. A bare count and a percentage of the range said
  // nothing on a time-budgeted run, and nothing about when it would end.
  function progressLine(n) {
    const secs = (Date.now() - runStart) / 1000;
    const rate = secs > 0.5 ? n / secs : 0;
    const userBudget = runBudget && !runBudgetDefault;
    const parts = [`scanned ${fmtN(n)}` + (userBudget ? '' : ` of ${fmtN(scanRange)}`) + ' seeds',
                   fmtT(secs) + ' elapsed'];
    if (rate) parts.push(fmtN(rate) + ' seeds/s');
    let left = (!userBudget && scanRange && rate) ? (scanRange - n) / rate : null;
    let capped = false;
    if (runBudget) {
      const b = runBudget - secs;
      if (left == null || b < left) { left = b; capped = runBudgetDefault; }
    }
    if (left != null) parts.push('~' + fmtT(left) + ' left'
      + (capped ? ' (stops there: no time budget set)' : ''));
    parts.push(liveSeeds.length + ' found');
    $('#scrollMeta').textContent = parts.join(' · ');
  }

  // Read the newline-delimited stream from /api/stream: {"budget"} first, then
  // {"t":seed} heartbeats and {"hit":{...}} as each seed is found, then a final
  // {"done":{...}} (or {"error":...}).
  // ---- stopping a run ------------------------------------------------------
  //
  // A run is a chain of requests (Gemini, then the search), so "stop" has to
  // survive across them: one controller per run, aborted by the Stop button.
  // Aborting the fetch is only half of it -- the search process on the other end
  // keeps burning CPU until someone kills it, so we also tell the server the job
  // id to terminate. Everything is keyed on `runToken`, which changes on every
  // new prompt, so a late reply from an abandoned run can never overwrite the
  // results of the current one.
  let runCtl = null, runJob = null, runToken = 0;

  function beginRun() {
    abortRun();                       // a new ask always replaces the old one
    runCtl = new AbortController();
    runJob = 'j' + Date.now() + Math.random().toString(36).slice(2, 8);
    return ++runToken;
  }
  function abortRun() {
    if (runCtl) { try { runCtl.abort(); } catch (_) {} }
    if (runJob) {
      // Fire-and-forget: if the job already finished the server no-ops, and a
      // failed cancel must not surface as an error on top of the real result.
      fetch('/api/cancel', { method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ job: runJob }) }).catch(() => {});
    }
    runCtl = null; runJob = null;
  }
  function endRun() { runCtl = null; runJob = null; }
  const isAbort = e => e && (e.name === 'AbortError' || /abort/i.test(e.message || ''));

  // Stop keeps what was found. The hits stream in as they happen, so a search
  // stopped at minute 29 of 30 still has everything it found by then -- before,
  // results existed only in the final report and Stop threw them all away.
  $('#stopBtn').onclick = () => {
    const kept = liveSeeds.slice();
    abortRun(); stopScroll(); setBusyUI(false);
    aiNote('stopped.');
    if (kept.length) {
      renderResults({ seeds: kept, funnel: { stopped: true }, notes: [] });
    } else {
      out.innerHTML = '';
      out.append(el('div', 'note warn',
        'Search stopped before it found anything. Ask something else, or press ➜ to run it again.'));
    }
    announce('search stopped' + (kept.length ? `, ${kept.length} seeds kept` : ''));
  };

  async function streamSearch(query, onTick, onHit, extra) {
    const resp = await fetch('/api/stream', {
      signal: runCtl ? runCtl.signal : undefined,
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      // `uppers` rides on the query rather than a control: it is a property of
      // what is being asked for (a rare biome condition needs more of a
      // structure seed's sister worlds tried), not something a user should have
      // to know to set. The planner emits it; without forwarding it here the
      // planner could ask and nothing would happen.
      body: JSON.stringify(Object.assign({ query, job: runJob,
                             range: +$('#range').value || 3000000,
                             seconds: budgetSeconds(),
                             uppers: query.uppers || 0,
                             threads: +$('#threads').value || 16 }, extra || {})) });
    if (!resp.ok) {
      const t = await resp.text();
      let m = t; try { m = JSON.parse(t).error || t; } catch (_) {}
      throw new Error(m || ('HTTP ' + resp.status));
    }
    const reader = resp.body.getReader(), dec = new TextDecoder();
    let buf = '', result = null;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl); buf = buf.slice(nl + 1);
        if (!line.trim()) continue;
        let msg; try { msg = JSON.parse(line); } catch (_) { continue; }
        if (msg.t !== undefined) onTick(msg.t, msg.n);
        else if (msg.hit) onHit(msg.hit);
        else if (msg.budget !== undefined) { runBudget = +msg.budget || 0; runBudgetDefault = !!msg.default_budget; }
        else if (msg.done) result = msg.done;
        else if (msg.cancelled) return null;   // stopped on purpose, not a failure
        else if (msg.error) throw new Error(msg.error);
      }
    }
    if (!result) throw new Error('the search ended without a result');
    return result;
  }

  // Minutes in the box -> seconds for the server. Capped at six hours, which is
  // also the server's ceiling; 0 means "no clock, use the seed count".
  const budgetSeconds = () =>
    Math.max(0, Math.min(360, +$('#budgetMin').value || 0)) * 60;

  // `extra` carries per-run options the controls do not hold -- the offset a
  // "continue" run resumes from.
  async function runSearch(query, inheritedToken, extra) {
    if (!query || !(query.conditions || []).length) { fail('nothing to search — add a condition'); return; }
    // A query may request a different MC version (e.g. something that only exists
    // on an older release). Switch the version selector and reload its vocab
    // before searching.
    if (query.version && query.version !== $('#version').value) {
      const opts = Array.from($('#version').options).map(o => o.value);
      if (opts.includes(query.version)) {
        $('#version').value = query.version;
        toast(`switched to MC ${query.version} for this search`);
        await loadVocab();
      }
    }
    // A budget from the natural-language layer drives the same controls the
    // user would have set by hand, so what runs is visible rather than hidden.
    if (query.budget) {
      if (query.budget.minutes) $('#budgetMin').value = query.budget.minutes;
      if (query.budget.seeds)   $('#range').value = query.budget.seeds;
      delete query.budget;                  // not part of the engine's schema
    }
    // reflect into the builder so result rows resolve their icons and the user
    // can open settings and tweak. The query itself is kept as loaded and runs
    // unchanged until the builder is edited (see buildQuery).
    loadQueryIntoBuilder(query);
    reveal();
    // A prompt hands down its own token (the Gemini call and the search are one
    // run); a direct Search press starts a fresh one, which also stops anything
    // still running.
    const token = (inheritedToken !== undefined) ? inheritedToken : beginRun();
    setBusyUI(true); startScroll(); out.innerHTML = '';
    scanRange = Math.max(1, Math.min(500000000, +$('#range').value || 3000000));
    runStart = Date.now(); runBudget = budgetSeconds(); runBudgetDefault = !runBudget;
    liveSeeds = []; liveCard = null;
    shownQuery = query;
    out.append(understoodCard(query));
    announce('searching');
    try {
      const d = await streamSearch(query,
        (s, n) => { if (token === runToken) pushSeed(s, false, n); },
        hit => {
          if (token !== runToken) return;
          liveSeeds.push(hit); pushSeed(hit.seed, true); showLive();
        }, extra);
      if (token !== runToken) return;     // superseded by a newer ask
      stopScroll();
      if (!d) return;                     // stopped; the Stop handler owns the UI
      renderResults(d);
      announce(`${(d.seeds || []).length} seeds found`);
    } catch (e) {
      stopScroll();
      if (isAbort(e) || token !== runToken) return;
      // A failure part-way does not erase what was already found.
      if (liveSeeds.length)
        renderResults({ seeds: liveSeeds.slice(), funnel: {}, notes: [], partialError: e.message });
      else fail(e.message);
    } finally {
      if (token === runToken) { setBusyUI(false); endRun(); }
    }
  }

  // Results as they arrive, replaced by the final report when the run ends.
  function showLive() {
    const c = seedsCard(liveSeeds,
      `Found so far: ${liveSeeds.length} — still searching; Stop keeps these`);
    if (liveCard && liveCard.parentNode) liveCard.replaceWith(c); else out.append(c);
    liveCard = c;
  }

  // ---- what the search is looking for ------------------------------------
  // The natural-language path used to be a black box: the prompt was cleared
  // and what the model understood was visible only by opening Build. This puts
  // the interpretation at the top of every result, with ways to change it.
  const ofLabel = of => !of || of === 'origin' ? 'origin' : of === 'spawn' ? 'spawn' : of;
  function condText(c) {
    if (c.not) return 'NO ' + condText(Object.assign({}, c, { not: false }));
    const w = c.within !== undefined ? ` ≤${c.within} from ${ofLabel(c.of)}` : '';
    if (c.eyes !== undefined) return `end portal with ≥${c.eyes} eyes`;
    if (c.loot) return `${c.loot.count || 1}+ ${pretty(c.loot.item)} in a ${pretty(c.loot.structure)} chest${w}`;
    if (c.ore) return `${c.count || 1}+ ${pretty(c.ore)} ore${c.exposed ? ' (exposed)' : ''}${c.vein ? `, vein ≥${c.vein}` : ''} within ${c.within || 64}`;
    if (c.slime !== undefined) return `${c.slime}+ slime chunks within ${c.within || 128}`;
    if (c.biome_area) return `${pretty(c.biome_area)} covering ${c.pct || 50}%+ within ${c.within || 800}`;
    if (c.island) return `island spawn, ${c.pct || 75}%+ ocean`;
    if (c.stronghold !== undefined) return `stronghold${c.max_depth !== undefined ? ` ≤${c.max_depth} blocks under the ground` : ''}${c.land === false ? ' (sea floor allowed)' : ''}${w}`;
    if (Array.isArray(c.overlap)) return `${pretty(c.overlap[0])} overlapping a ${pretty(c.overlap[1])}${w}`;
    if (c.height !== undefined || c.relief !== undefined)
      return (c.relief ? `${c.relief}+ blocks of relief` : `terrain ≥ y${c.height}`) + `${c.exact ? ' (exact)' : ''} within ${c.within}`;
    if (c.cactus !== undefined) return `cactus ≥${c.cactus} tall within ${c.within}`;
    if (c.structure) return `${(c.count || 1) > 1 ? c.count + '× ' : ''}${pretty(c.structure)}${w}`;
    if (c.biome) return `${pretty(c.biome)} biome${w}`;
    return c.id;
  }
  function understoodCard(query) {
    const box = el('div', 'understood');
    box.append(el('span', 'lbl', `Java ${query.version || $('#version').value}${query.large_biomes ? ' · Large Biomes' : ''} · looking for:`));
    (query.conditions || []).forEach(c => {
      const chip = el('span', 'cond', condText(c));
      chip.title = `condition "${c.id}"`;
      box.append(chip);
    });
    if (query.rank) box.append(el('span', 'cond', `ranked by ${query.rank.of} (top ${query.rank.top || 10})`));
    const edit = el('button', 'tiny', 'edit'); edit.type = 'button';
    edit.onclick = () => openDrawer('build');
    const est = el('button', 'tiny', 'estimate rarity'); est.type = 'button';
    est.onclick = () => $('#explainBtn').click();
    const sh = el('button', 'tiny', 'share'); sh.type = 'button';
    sh.onclick = () => shareLink(sh);
    box.append(edit, est, sh);
    return box;
  }

  // ---- copying -----------------------------------------------------------
  // Awaited, and honest about failure: "copied" used to appear even when the
  // clipboard refused (non-secure context, denied permission).
  async function copyText(btn, text) {
    const was = btn.textContent;
    try { await navigator.clipboard.writeText(text); btn.textContent = '✓'; announce('copied'); }
    catch (_) { btn.textContent = '✗'; toast('could not copy — select the text and copy it by hand'); }
    setTimeout(() => { btn.textContent = was; }, 900);
  }
  // A teleport that works from wherever the player is standing. A plain
  // "/tp @s x ~ z" for a fortress teleports to those coordinates in the
  // OVERWORLD if that is where you are.
  function tpCommand(x, z, dim, y) {
    const yy = (y === undefined || y === null) ? '~' : y;
    if (!dim || dim === 'overworld') return `/tp @s ${x} ${yy} ${z}`;
    const id = dim === 'nether' ? 'minecraft:the_nether' : 'minecraft:the_end';
    return `/execute in ${id} run tp @s ${x} ${yy} ${z}`;
  }

  // ---- results ------------------------------------------------------------
  function seedsCard(seeds, title) {
    const c = el('div', 'card');
    c.append(el('h3', null, title || 'Seeds — click a number to copy'));
    c.append(exportBar(seeds));
    // Result rows carry the condition id (e.g. "s5"); map it back to the
    // structure name so the row shows the right icon, not a generic pin.
    const condName = {};
    conds.forEach(x => { if (x.kind === 'structure') condName[x.id] = x.name; });
    seeds.forEach(s => {
      const box = el('div', 'seed');
      const n = el('div', 'n');
      const num = el('span', 'num', s.seed);
      num.title = 'copy';
      num.onclick = () => copySeed(num, s.seed);
      const mapb = el('button', 'tiny', '🗺 map');
      mapb.onclick = () => mapView(s.seed);
      const desc = el('button', 'tiny', 'describe');
      desc.onclick = () => describe(s.seed);
      const rec = el('button', 'tiny', 'copy as record');
      rec.title = 'seed, version, every coordinate with its /tp, the value -- the fields a seedfinding record lists';
      rec.onclick = () => copyText(rec, recordText(s));
      n.append(num, mapb, desc, rec);
      // Ranked runs carry the score that put this seed on the board.
      if (s.score !== undefined)
        n.append(el('span', 'dim', `${s.metric} ${s.score.toLocaleString()}`));
      box.append(n);
      const t = el('table');
      // Every coordinate in a result opens the map there. Reading "x=1424
      // z=736" and then panning to it by hand is the step that made results
      // feel unverifiable. Nether and End rows say so, carry the right /tp,
      // and a nether row gives the overworld spot a portal there leads to.
      const goCell = (x, z, structure, y) => {
        const td = el('td');
        const dim = structure ? (vocab.structures[structure] || 'overworld') : 'overworld';
        if (dim !== 'overworld') td.append(el('span', 'dimtag ' + dim, dim === 'nether' ? 'NETHER' : 'END'));
        const link = el('button', 'coord',
          `x=${x}  z=${z}` + (y !== undefined && y !== null ? `  y=${y}` : ''));
        link.type = 'button';
        link.title = 'show this on the map';
        link.onclick = () => mapView(s.seed, { cx: x, cz: z, dim, radius: 600 });
        const cp = el('button', 'tiny', '⧉');
        cp.type = 'button'; cp.title = 'copy coordinates';
        cp.setAttribute('aria-label', `copy coordinates ${x} ${z}`);
        cp.style.marginLeft = '6px';
        cp.onclick = () => copyText(cp, `${x} ${z}`);
        const tp = el('button', 'tiny', '/tp');
        tp.type = 'button'; tp.title = 'copy a teleport command for this spot, in the right dimension';
        tp.onclick = () => copyText(tp, tpCommand(x, z, dim, y));
        td.append(link, cp, tp);
        if (dim === 'nether') td.append(el('span', 'dim', `  ≈ overworld ${x * 8}, ${z * 8}`));
        return td;
      };
      const row = (idc, cell, detail) => {
        const tr = el('tr'); tr.append(idc, cell, el('td', 'd', detail)); t.append(tr);
      };
      if (s.spawn) row(idCell('spawn', '', 'Spawn'), goCell(s.spawn.x, s.spawn.z), '');
      if (s.portal) row(idCell('eyes', '', 'End Portal'), goCell(s.portal.x, s.portal.z),
                        s.portal.eyes + '/12 eyes');
      (s.loot || []).forEach(l => row(idCell('item', l.item, pretty(l.id)), goCell(l.x, l.z),
        `${l.count} ${pretty(l.item)}`));
      (s.ore || []).forEach(o => {
        let detail = `${o.count.toLocaleString()}${o.exposed ? ' exposed' : ''} within ${o.within}`;
        if (o.vein !== undefined) detail += ` · biggest vein ${o.vein}`;
        row(idCell('item', o.material, pretty(o.material)), goCell(o.x, o.z), detail);
      });
      (s.overlap || []).forEach(o => {
        row(idCell('structure', o.structure, pretty(o.structure)), goCell(o.x, o.z, o.structure),
            o.gap !== undefined ? `${o.gap} blocks apart` : '');
        if (o.structure2 !== undefined)
          row(idCell('structure', o.structure2, pretty(o.structure2)),
              goCell(o.x2, o.z2, o.structure2), `overlaps ${o.area} blocks²`);
      });
      (s.stronghold || []).forEach(o => {
        // The roof AND the ground over it -- a roof at y=60 is a metre down on
        // a plain and buried in a mountain -- and the /tp goes to the
        // shallowest piece, which can be 100+ blocks from the start chunk.
        row(idCell('structure', 'stronghold', 'Stronghold'), goCell(o.x, o.z, 'stronghold'),
            `roof y=${o.roof}, ${o.state}`);
        if (o.tp) row(idCell('structure', 'stronghold', 'Shallowest part'),
                      goCell(o.tp.x, o.tp.z, 'stronghold', o.tp.y),
                      `${o.depth} block(s) of ground over it, ${o.where}`
                      + (o.tall ? ' — tall stronghold: heights right ~2 in 3, verify' : ''));
      });
      (s.cactus || []).forEach(o => row(idCell('item', 'cactus', 'Cactus'), goCell(o.x, o.z),
        `${o.height} blocks tall, base y=${o.base_y}`));
      (s.slime || []).forEach(o => row(idCell('slime', '', 'Slime Chunks'), goCell(o.x, o.z),
        `${o.count.toLocaleString()} within ${o.within}`));
      (s.area || []).forEach(o => row(idCell('biome', o.biome, pretty(o.biome)), goCell(o.x, o.z),
        `${o.pct}% within ${o.within}`));
      (s.island || []).forEach(o => {
        const c2 = el('td', 'id'); c2.append(el('span', 'ic', '🏝️'), document.createTextNode('Island Spawn'));
        row(c2, goCell(o.x, o.z), `${o.pct}% ocean within ${o.within}`);
      });
      (s.height || []).forEach(o => {
        // Exact terrain is a measurement; the default is an estimate. Say
        // which one produced the number.
        const t2 = o.exact ? '' : '~', tag = o.exact ? ' (exact)' : ' (approx)';
        const detail = o.relief !== undefined
          ? `${t2}${o.relief} relief, ${t2}${o.peak} peak${tag}`
          : `${t2}${o.peak} peak${tag}`;
        row(idCell('height', '', o.relief !== undefined ? 'Terrain Relief' : 'Terrain Height'),
            goCell(o.x, o.z), detail);
      });
      (s.biome || []).forEach(o => row(idCell('biome', o.biome, pretty(o.biome)), goCell(o.x, o.z),
        `${o.dist} from ${o.ref}`));
      (s.cluster || []).forEach(o => row(
        idCell('structure', o.structure, `${o.count}× ${pretty(o.structure)}`),
        goCell(o.x, o.z, o.structure),
        o.tight ? `${o.count} within ${o.spread} of each other` : `${o.count} within ${o.within}`));
      (s.absent || []).forEach(a => {
        const td = el('td', 'd', '—');
        row(idCell('structure', '', 'None: ' + a.id), td, `none within ${a.within} of ${a.of}`);
      });
      (s.places || []).forEach(p => {
        // The engine names the structure now; the builder lookup is the
        // fallback for results from an older find.exe.
        const nm = p.structure || condName[p.id] || p.id;
        let detail = p.dist + ' from ' + (p.ref || 'origin') + (p.size ? `, size ${p.size}` : '');
        if (p.cave) detail += `, ${p.cave}-block cave below`;
        if (p.floating) detail += `, floating island at (${p.floating.x},${p.floating.z}): `
                                + `${p.floating.gap}-block gap, underside y=${p.floating.underside}`;
        row(idCell('structure', nm, pretty(nm)), goCell(p.x, p.z, nm), detail);
      });
      if (t.childElementCount) box.append(t);
      c.append(box);
    });
    return c;
  }
  // A find written up the way seedfinding records are listed (seed, version,
  // coordinates, value, finder), ready to paste into a post or the records
  // wiki. The version is the one SEARCHED; whether it holds on other versions
  // was not tested, and the text says so rather than implying a range.
  function recordText(s) {
    const q = shownQuery || {};
    const v = q.version || $('#version').value;
    const lines = [
      `Seed: ${s.seed}`,
      `Edition: Java · version searched: ${v}${q.large_biomes ? ' · Large Biomes' : ''}`
        + ' (other versions not checked)',
    ];
    if (s.score !== undefined) lines.push(`Value: ${s.metric} = ${s.score}`);
    if ((q.conditions || []).length)
      lines.push('Searched for: ' + q.conditions.map(condText).join('; '));
    lines.push('Where:');
    const rows = seedRows([s]).slice(1);          // [seed, kind, id, x, z, detail]
    const kindDim = r => {
      const st = r[1] === 'structure' || r[1] === 'cluster' || r[1] === 'overlap' ? r[2] : '';
      const nm = (conds.find(c => c.id === st) || {}).name || st;
      return vocab.structures[nm] || 'overworld';
    };
    rows.forEach(r => {
      if (r[3] === '' || r[3] === undefined) return;
      const dim = kindDim(r);
      lines.push(`  - ${r[2] || r[1]}${dim !== 'overworld' ? ' (' + dim + ')' : ''}: `
        + `x=${r[3]} z=${r[4]}${r[5] ? ' -- ' + r[5] : ''}   ${tpCommand(r[3], r[4], dim)}`);
    });
    lines.push('Finder: ');
    lines.push(`Found with SeedGPT (cubiomes engine) on ${new Date().toISOString().slice(0, 10)}.`
      + ' Positions are exact engine output; verify in game before claiming a record.');
    return lines.join('\n');
  }

  function copySeed(span, seed) {
    (navigator.clipboard ? navigator.clipboard.writeText(seed) : Promise.reject()).then(() => {
      span.textContent = 'copied'; announce('seed copied');
    }, () => { span.textContent = 'copy failed'; })
      .finally(() => setTimeout(() => { span.textContent = seed; }, 800));
  }

  // What to do next, offered as buttons -- above all when nothing was found,
  // which used to get one generic sentence and no way forward.
  function nextSteps(d) {
    const f = d.funnel || {};
    const box = el('div', 'next-steps');
    const q = lastResult && lastResult.query;
    if (q && f.resume_offset !== undefined && (f.timed_out || !d.seeds.length)) {
      const cont = el('button', 'tiny', 'continue with the next seeds');
      cont.title = 'scan fresh seeds starting where this run stopped -- nothing is scanned twice';
      cont.onclick = () => runSearch(JSON.parse(JSON.stringify(q)), undefined,
                                     { offset: f.resume_offset });
      box.append(cont);
    }
    if (q && !d.seeds.length) {
      const more = el('button', 'tiny', 'search 10× more seeds');
      more.onclick = () => {
        $('#range').value = Math.min(500000000, (+$('#range').value || 3000000) * 10);
        runSearch(JSON.parse(JSON.stringify(q)));
      };
      const est = el('button', 'tiny', 'estimate how rare this is');
      est.onclick = () => $('#explainBtn').click();
      const edit = el('button', 'tiny', 'loosen a condition');
      edit.onclick = () => openDrawer('build');
      box.append(more, est, edit);
    }
    return box;
  }

  function renderResults(d) {
      const f = d.funnel || {};
      d.seeds = d.seeds || [];
      lastResult = { d, query: shownQuery };
      out.innerHTML = '';
      if (shownQuery) out.append(understoodCard(shownQuery));
      out.append(...noteCards(d.notes));   // caveats first: they qualify what follows

      if (f.scanned !== undefined)
        out.append(statsCard('Result', [
          ['seeds found', String(d.seeds.length), d.seeds.length ? 'good' : 'warn'],
          ['scanned', (f.scanned ?? 0).toLocaleString()],
          ['elapsed', (f.seconds ?? 0) + ' s'],
          ['throughput', (f.mseeds_s ?? 0) + ' M/s'],
        ]));
      if (f.stopped)
        out.append(el('div', 'note warn',
          `Stopped by you. These are the ${d.seeds.length} seed(s) found before then — the scan did not finish.`));
      if (d.partialError)
        out.append(el('div', 'note bad',
          `The search failed part-way (${d.partialError}). These ${d.seeds.length} seed(s) were found before it did.`));
      // "Best of N" has to name the N that was actually reached, not the one
      // that was requested before the clock ran out.
      if (f.timed_out && f.budget_was_default)
        out.append(el('div', 'note warn',
          `Stopped after ${fmtT(f.budget_s || 0)}, the limit for a search without a time budget, `
          + `having scanned ${(f.scanned ?? 0).toLocaleString()} seeds — not the whole range. `
          + `Set a time budget under Build to let it run longer, or continue with the next seeds below.`));
      else if (f.timed_out)
        out.append(el('div', 'note',
          `Time budget reached — this is the best of the ${(f.scanned ?? 0).toLocaleString()} seeds actually scanned, not the full range. Give it longer for better results.`));
      if (f.killed)
        out.append(el('div', 'note bad',
          'The server had to force-stop this search, so its counts are incomplete.'));
      // World spawn is an estimate. find.exe prints this caveat, but its output
      // is parsed as JSON so the line never reached anyone -- and a
      // spawn-relative result is the one a player checks IMMEDIATELY, by loading
      // the world and looking. Rendered from the query instead of the stream.
      if (((shownQuery && shownQuery.conditions) || []).some(c => c && c.of === 'spawn'))
        out.append(el('div', 'note warn',
          'Distances from WORLD SPAWN are approximate. The spawn point depends on '
          + 'grass placement, which is decoration and is not simulated here, so the '
          + 'real in-game spawn can sit some way off. Structure positions are exact; '
          + 'their distance from spawn is not.'));

      if (f.pass1_pct != null) {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Funnel'));
        const bar = el('div', 'funnel'), i = el('i');
        i.style.width = Math.max(1.5, Math.min(100, f.pass1_pct)) + '%';
        if (f.pass1_pct > 50) i.style.background = 'var(--gold)';
        bar.append(i); c.append(bar);
        c.append(el('div', 'funnel-l',
          `pass 1 kept ${f.pass1_pct}% (${(f.pass1 || 0).toLocaleString()}) · pass 2 confirmed ${(f.pass2 || 0).toLocaleString()}`));
        if (f.too_loose) c.append(el('div', 'note warn',
          "Pass 1 isn't filtering — every seed paid full biome cost. Tighten a radius."));
        out.append(c);
      }

      if (!d.seeds.length) {
        out.append(el('div', 'note warn',
          f.stopped || f.killed ? 'Nothing was found before the search stopped.'
            : 'No seeds in what was scanned — which is not the same as none existing.'));
      } else {
        out.append(seedsCard(d.seeds));
      }
      out.append(nextSteps(d));
      if (d.plan) out.append(planCard(d.plan));
      if (d.raw) out.append(rawCard(d.raw));
  }

  // Back from a map or a describe: show the results that were already there.
  function showLastResults() {
    if (lastResult) { shownQuery = lastResult.query; renderResults(lastResult.d); }
    else $('#searchBtn').click();
  }

  $('#searchBtn').onclick = () => { if (!guard()) return; closeDrawer(); runSearch(buildQuery()); };

  // Check a list of seeds against the builder's conditions (/api/check).
  $('#checkBtn').onclick = async () => {
    if (!guard()) return;
    const text = $('#checkSeeds').value.trim();
    if (!text) { toast('paste some seeds first'); $('#checkSeeds').focus(); return; }
    const query = buildQuery();
    busy('checking your seeds…');
    const btn = $('#checkBtn'); btn.disabled = true;
    try {
      const d = await post('/api/check', { query, seeds: text,
                                           threads: +$('#threads').value || 16 });
      shownQuery = query;
      out.innerHTML = '';
      out.append(understoodCard(query));
      out.append(statsCard('Seed check', [
        ['checked', String(d.checked)],
        ['have it', String(d.matched), d.matched ? 'good' : 'warn'],
        ['do not', String(d.checked - d.matched)],
      ]));
      const yes = d.results.filter(r => r.verdict === 'yes' && r.hit);
      if (yes.length) out.append(seedsCard(yes.map(r => r.hit), `These ${yes.length} have it`));
      const no = d.results.filter(r => r.verdict !== 'yes');
      if (no.length) {
        const c = el('div', 'card');
        c.append(el('h3', null, `These ${no.length} do not`));
        const t = el('table');
        no.slice(0, 500).forEach(r => {
          const tr = el('tr');
          const label = r.input === r.seed ? r.seed : `${r.input} → ${r.seed}`;
          tr.append(el('td', 'id', label), el('td', 'd', r.why || r.verdict));
          t.append(tr);
        });
        c.append(t);
        if (no.length > 500) c.append(el('div', 'hint', `…and ${no.length - 500} more.`));
        out.append(c);
      }
      announce(`${d.matched} of ${d.checked} seeds have it`);
    } catch (e) { fail(e.message); }
    finally { btn.disabled = false; }
  };

  const MARKERS = [
    ['mansion','#FF4646'], ['village','#FFD23C'], ['monument','#50BEFF'],
    ['ancient city','#BE6EFF'], ['outpost','#FF8C28'], ['desert pyramid','#FAF0A0'],
    ['jungle temple','#78DC78'], ['swamp hut','#966EC8'], ['igloo','#E6F5FF'],
  ];

  // Interactive pan/zoom biome map (Chunkbase-style). Drag pans (re-fetches a
  // re-centred tile from map.exe), wheel zooms (changes radius), click reads
  // out block coordinates. Structure markers are baked into the PNG by map.c.
  // opts: {cx, cz, dim} -- opening a map ON a result is the whole point of
  // having one, so every result row can call this with its own coordinates.
  function mapView(seed, opts) {
    const PX = 560;
    const o = opts || {};
    let cx = o.cx || 0, cz = o.cz || 0, radius = o.radius || 2000;
    let dim = o.dim || 'overworld';
    const v = $('#version').value;
    reveal(); out.innerHTML = '';
    const card = el('div', 'card');
    card.append(el('h3', null, '🗺 Map · world ' + seed));

    const bar = el('div', 'mapctl');
    const info = el('span');
    info.style.cssText = 'font-family:var(--mono);font-size:11px;color:var(--ink-soft)';
    const zout = el('button', 'tiny', '－'), zin = el('button', 'tiny', '＋'),
          home = el('button', 'tiny', '⌂ spawn');
    bar.append(zout, zin, home);
    // Dimension switch. Nether coordinates are 1:8, so following a portal means
    // dividing by 8 -- do that on the switch rather than dumping the player at
    // a place eight times too far out.
    const dimBtns = {};
    [['overworld','🌳 over'], ['nether','🔥 nether'], ['end','🌌 end']].forEach(([d, lbl]) => {
      const b = el('button', 'tiny', lbl);
      b.onclick = () => {
        if (dim === d) return;
        const f = (dim === 'nether' ? 8 : 1) / (d === 'nether' ? 8 : 1);
        cx = Math.round(cx * f); cz = Math.round(cz * f);
        dim = d; syncDim(); load();
      };
      dimBtns[d] = b; bar.append(b);
    });
    function syncDim() {
      Object.entries(dimBtns).forEach(([d, b]) =>
        b.style.outline = (d === dim) ? '2px solid var(--grass)' : '');
    }
    syncDim();
    bar.append(info); card.append(bar);

    const vp = el('div');
    vp.style.cssText = `position:relative;width:${PX}px;max-width:100%;aspect-ratio:1/1;`
      + 'overflow:hidden;border:3px solid var(--slot-edge);cursor:grab;'
      + 'image-rendering:pixelated;background:var(--slot);touch-action:none';
    const img = el('img'); img.alt = 'Biome map';
    img.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;'
      + 'user-select:none;-webkit-user-drag:none;pointer-events:none';
    const read = el('div');
    read.style.cssText = 'position:absolute;left:6px;bottom:6px;font-family:var(--mono);'
      + 'font-size:11px;background:rgba(0,0,0,.6);color:#fff;padding:2px 6px;'
      + 'pointer-events:none;display:none';
    // Clickable markers, drawn over the baked-in ones from the same list
    // (/api/markers). Real buttons: reachable by keyboard, announced by name.
    const layer = el('div');
    layer.style.cssText = 'position:absolute;inset:0;pointer-events:none';
    vp.append(img, layer, read); card.append(vp);

    const lg = el('div', 'legend');
    (typeof MARKERS !== 'undefined' ? MARKERS : []).forEach(([nm, col]) => {
      const sp = el('span'), sw = el('i'); sw.style.background = col;
      sp.append(sw, document.createTextNode(pretty(nm))); lg.append(sp);
    });
    { const sp = el('span'), sw = el('i'); sw.style.background = '#F0F0F0';
      sp.append(sw, document.createTextNode('Stronghold')); lg.append(sp); }
    const sc = el('span'); sc.append(document.createTextNode('✛ spawn')); lg.append(sc);
    card.append(lg);
    card.append(el('div', 'hint', 'Drag to pan · scroll to zoom · click a marker for its coordinates and /tp.'));
    const pick = el('div', 'note'); pick.hidden = true;
    const inview = el('div', 'next-steps');
    card.append(pick, inview);
    const back = el('button', 'tiny', '← back to results');
    back.onclick = showLastResults;
    card.append(back); out.append(card);

    // Every pan or zoom renders a whole PNG on the server, so requests are
    // debounced (a fast wheel spin used to queue a dozen renders, all but the
    // last wasted), and the view says when it is loading or failed.
    let loadTimer = null;
    img.onload  = () => { info.textContent = `${dim} · centre ${cx}, ${cz}  ·  ±${radius} blocks`; };
    img.onerror = () => { info.textContent = 'map render failed — try again, or zoom out'; };
    function load() {
      clearTimeout(loadTimer);
      info.textContent = `loading ${dim} · centre ${cx}, ${cz}  ·  ±${radius} blocks…`;
      loadTimer = setTimeout(() => {
        const common = `seed=${encodeURIComponent(seed)}&v=${encodeURIComponent(v)}`
          + (largeBiomes() ? '&large=1' : '')
          + (dim !== 'overworld' ? `&dim=${dim}` : '')
          + `&r=${radius}&cx=${cx}&cz=${cz}`;
        img.src = `/api/map?${common}&px=${PX}`;
        const view = { cx, cz, radius, dim, tok: ++markTok };
        fetch(`/api/markers?${common}`).then(r => r.ok ? r.json() : null)
          .then(d => { if (d && view.tok === markTok) drawMarkers(d, view); })
          .catch(() => {});
      }, 180);
    }
    let markTok = 0;
    function drawMarkers(d, view) {
      layer.innerHTML = ''; inview.innerHTML = '';
      const span = 2 * view.radius;
      const ms = (d.markers || []).slice();
      if (d.spawn) ms.push({ type: 'spawn', x: d.spawn.x, z: d.spawn.z });
      const counts = {};
      ms.forEach(m => {
        counts[m.type] = (counts[m.type] || 0) + 1;
        const b = el('button');
        const label = `${pretty(m.type)} at ${m.x}, ${m.z}`;
        b.type = 'button'; b.title = label; b.setAttribute('aria-label', label);
        b.style.cssText = 'position:absolute;width:16px;height:16px;padding:0;'
          + 'transform:translate(-50%,-50%);border-radius:50%;border:2px solid rgba(255,255,255,.85);'
          + 'background:rgba(0,0,0,.15);cursor:pointer;pointer-events:auto;'
          + `left:${((m.x - (view.cx - view.radius)) / span) * 100}%;`
          + `top:${((m.z - (view.cz - view.radius)) / span) * 100}%`;
        b.onpointerdown = e => e.stopPropagation();    // a click, not a drag
        b.onclick = e => { e.stopPropagation(); selectMarker(m, view.dim); };
        layer.append(b);
      });
      Object.entries(counts).forEach(([t, n]) => {
        const chip = el('button', 'tiny', `${n} ${pretty(t)}`);
        chip.type = 'button';
        chip.title = `select the ${pretty(t).toLowerCase()} nearest the centre`;
        chip.onclick = () => {
          const best = ms.filter(m => m.type === t).sort((a, b) =>
            Math.hypot(a.x - view.cx, a.z - view.cz) - Math.hypot(b.x - view.cx, b.z - view.cz))[0];
          if (best) selectMarker(best, view.dim);
        };
        inview.append(chip);
      });
    }
    function selectMarker(m, mdim) {
      pick.hidden = false; pick.innerHTML = '';
      pick.append(el('b', null, pretty(m.type)), document.createTextNode(`  x=${m.x}  z=${m.z}  `));
      const cp = el('button', 'tiny', '⧉ copy'); cp.type = 'button';
      cp.onclick = () => copyText(cp, `${m.x} ${m.z}`);
      const tp = el('button', 'tiny', '/tp'); tp.type = 'button';
      tp.onclick = () => copyText(tp, tpCommand(m.x, m.z, mdim));
      const go = el('button', 'tiny', 'centre here'); go.type = 'button';
      go.onclick = () => { cx = m.x; cz = m.z; load(); };
      pick.append(cp, tp, go);
      if (mdim === 'nether') pick.append(el('span', 'dim', `  ≈ overworld ${m.x * 8}, ${m.z * 8}`));
      announce(`${pretty(m.type)} at ${m.x}, ${m.z}`);
    }
    zin.onclick  = () => { radius = Math.max(200, Math.round(radius / 2)); load(); };
    zout.onclick = () => { radius = Math.min(20000, radius * 2); load(); };
    zin.setAttribute('aria-label', 'zoom in'); zout.setAttribute('aria-label', 'zoom out');
    home.onclick = () => { cx = 0; cz = 0; radius = 2000; dim = 'overworld'; syncDim(); load(); };

    let drag = null, moved = false;
    vp.onpointerdown = e => { drag = { x: e.clientX, y: e.clientY }; moved = false;
      vp.setPointerCapture(e.pointerId); vp.style.cursor = 'grabbing'; };
    vp.onpointermove = e => {
      if (!drag) {
        // Hover readout: where the pointer is, in blocks, without clicking.
        const rect = vp.getBoundingClientRect();
        const bx = Math.round(cx + ((e.clientX - rect.left) / rect.width - 0.5) * 2 * radius);
        const bz = Math.round(cz + ((e.clientY - rect.top) / rect.height - 0.5) * 2 * radius);
        read.style.display = 'block'; read.textContent = `x ${bx}, z ${bz}`;
        return;
      }
      const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
      if (Math.abs(dx) > 3 || Math.abs(dy) > 3) moved = true;
      img.style.transform = layer.style.transform = `translate(${dx}px,${dy}px)`; };
    vp.onpointerleave = () => { if (!drag) read.style.display = 'none'; };
    vp.onpointerup = e => { if (!drag) return;
      const dx = e.clientX - drag.x, dy = e.clientY - drag.y; drag = null;
      vp.style.cursor = 'grab'; img.style.transform = layer.style.transform = '';
      if (moved) { const rw = vp.getBoundingClientRect().width || PX, span = 2 * radius;
        cx -= Math.round(dx / rw * span); cz -= Math.round(dy / rw * span); load(); } };
    // Zoom about the cursor, the way every map does: the point under the
    // pointer stays put instead of everything rushing to the centre.
    vp.onwheel = e => { e.preventDefault();
      const rect = vp.getBoundingClientRect();
      const fx = (e.clientX - rect.left) / rect.width - 0.5;
      const fz = (e.clientY - rect.top) / rect.height - 0.5;
      const px = cx + fx * 2 * radius, pz = cz + fz * 2 * radius;
      radius = e.deltaY > 0 ? Math.min(20000, radius * 2) : Math.max(200, Math.round(radius / 2));
      cx = Math.round(px - fx * 2 * radius); cz = Math.round(pz - fz * 2 * radius);
      load(); };
    vp.onclick = e => { if (moved) return;
      const rect = vp.getBoundingClientRect();
      const bx = Math.round(cx + ((e.clientX - rect.left) / rect.width - 0.5) * 2 * radius);
      const bz = Math.round(cz + ((e.clientY - rect.top) / rect.height - 0.5) * 2 * radius);
      read.style.display = 'block'; read.textContent = `x ${bx}, z ${bz}`; };
    load();
  }

  async function describe(seed, radius) {
    radius = radius || 2000;
    busy('reading world ' + seed + '…');
    try {
      const d = await post('/api/describe',
        { seed, version: $('#version').value, radius, large_biomes: largeBiomes() });
      out.innerHTML = '';
      const c = el('div','card');
      c.append(el('h3', null, 'World ' + seed));

      // zoom control -- re-renders map and structure list together
      const ctl = el('div','mapctl');
      ctl.append(el('label', null, 'radius'));
      const zoom = el('select');
      [500, 1000, 2000, 4000, 8000].forEach(r => {
        const o = el('option', null, r.toLocaleString() + ' blocks'); o.value = r;
        if (r === radius) o.selected = true; zoom.append(o);
      });
      zoom.onchange = e => describe(seed, +e.target.value);
      ctl.append(zoom);
      c.append(ctl);

      const wrap = el('div','map');
      const img = el('img');
      img.alt = `Biome map of seed ${seed}, ${radius} blocks around spawn`;
      img.src = `/api/map?seed=${encodeURIComponent(seed)}`
        + (largeBiomes() ? '&large=1' : '')
              + `&v=${encodeURIComponent($('#version').value)}&r=${radius}&px=560`;
      img.onerror = () => { wrap.replaceChildren(
        el('div','note warn','map render failed — the structure list below is still valid')); };
      wrap.append(img);

      const lg = el('div','legend');
      MARKERS.forEach(([n, col]) => {
        const sp = el('span'); const sw = el('i');
        sw.style.background = col; sp.append(sw, document.createTextNode(pretty(n))); lg.append(sp);
      });
      const sp = el('span'); sp.append(document.createTextNode('✛ world spawn'));
      lg.append(sp);
      wrap.append(lg);
      c.append(wrap);

      // The world as a table: every structure type, nearest first, each row
      // with map, copy and /tp -- instead of a block of text to copy
      // coordinates out of. The text stays available below.
      const w = d.world;
      if (w) {
        const head = el('div', 'note');
        head.append(document.createTextNode(
          `Java ${w.version} · spawn x=${w.spawn.x} z=${w.spawn.z} in ${pretty(w.spawn.biome)} · `
          + `structure seed ${w.structure_seed} · ${w.structures.length} structures within ${w.radius}  `));
        const om = el('button', 'tiny', '🗺 interactive map'); om.type = 'button';
        om.onclick = () => mapView(seed, { cx: w.spawn.x, cz: w.spawn.z, radius: Math.min(20000, w.radius) });
        const st = el('button', 'tiny', '/tp spawn'); st.type = 'button';
        st.onclick = () => copyText(st, tpCommand(w.spawn.x, w.spawn.z, 'overworld'));
        head.append(om, st);
        c.append(head);
        const byType = {};
        w.structures.forEach(x => (byType[x.type] = byType[x.type] || []).push(x));
        const types = Object.keys(byType).sort((a, b) => byType[a][0].dist - byType[b][0].dist);
        const t = el('table');
        const rowFor = x => {
          const tr = el('tr');
          const td = el('td');
          const link = el('button', 'coord', `x=${x.x}  z=${x.z}`); link.type = 'button';
          link.title = 'show this on the map';
          link.onclick = () => mapView(seed, { cx: x.x, cz: x.z, radius: 600 });
          const cp = el('button', 'tiny', '⧉'); cp.type = 'button';
          cp.setAttribute('aria-label', `copy coordinates ${x.x} ${x.z}`);
          cp.onclick = () => copyText(cp, `${x.x} ${x.z}`);
          const tp = el('button', 'tiny', '/tp'); tp.type = 'button';
          tp.onclick = () => copyText(tp, tpCommand(x.x, x.z, 'overworld'));
          td.append(link, cp, tp);
          tr.append(idCell('structure', x.type, pretty(x.type)), td,
                    el('td', 'd', `${x.dist} from spawn · ${pretty(x.biome)}${x.note ? ' · ' + x.note : ''}`));
          return tr;
        };
        types.forEach(ty => {
          const list = byType[ty];
          list.slice(0, 3).forEach(x => t.append(rowFor(x)));
          if (list.length > 3) {
            const tr = el('tr'), td = el('td');
            td.colSpan = 3;
            const more = el('button', 'tiny', `+ ${list.length - 3} more ${pretty(ty).toLowerCase()}`);
            more.type = 'button';
            more.onclick = () => { const rows = list.slice(3).map(rowFor); tr.replaceWith(...rows); };
            td.append(more); tr.append(td); t.append(tr);
          }
        });
        c.append(t);
      }
      const det = el('details');
      det.append(el('summary', null, 'as text'));
      const p = el('pre'); p.textContent = d.raw; det.append(p);
      c.append(det);
      out.append(c);
      const back = el('button','tiny','← back to results');
      back.onclick = showLastResults;
      out.append(back);
    } catch (e) { fail(e.message); }
  }

  // ---- AI prompt (Gemini) + settings drawer ------------------------------
  // The key is stored PER PROVIDER, like the model.
  //
  // It used to be one slot shared by both, so saving an OpenRouter key and then
  // switching to Gemini sent "sk-or-v1-..." to Google, which answers "API key
  // not valid" -- the exact message you get from a revoked key. The advice that
  // follows ("make a new one") cannot work, because the new key is never the
  // one being sent.
  const LEGACY_KEY_LS = 'sf-gemini-key';
  const keyKey = p => 'sf-ai-key-' + p;
  const getKey = () => localStorage.getItem(keyKey(getProvider())) || '';
  function migrateLegacyKey() {
    const old = localStorage.getItem(LEGACY_KEY_LS);
    if (!old) return;
    // Hand it to whichever provider it plainly belongs to, rather than guessing
    // from whatever happens to be selected right now.
    const dest = /^AIza/.test(old) ? 'gemini' : 'compatible';
    if (!localStorage.getItem(keyKey(dest))) localStorage.setItem(keyKey(dest), old);
    localStorage.removeItem(LEGACY_KEY_LS);
  }
  migrateLegacyKey();
  function refreshKeyState() {
    const k = getKey();
    $('#keyState').textContent = k
      ? '✓ saved for ' + getProvider() + ' (…' + k.slice(-4) + ')'
      : 'no key for ' + getProvider() + ' yet';
    // The landing screen says the same thing in plain sight, so it has to
    // follow every save, clear and provider switch.
    refreshSetup();
  }
  $('#keySave').onclick = () => {
    const v = $('#geminiKey').value.trim();
    if (!v) { toast('paste a key first'); return; }
    const p = getProvider();
    // A Gemini key is AIza...; an OpenAI-style one is sk-.... Saying so at the
    // moment of the mistake beats a 400 from Google ten minutes later.
    if (p === 'gemini' && /^sk-/.test(v)) {
      toast('that looks like an OpenAI/OpenRouter key — switch the provider to OpenAI-compatible first');
      return;
    }
    // Only the sk- case is checked. Google issues both "AIza..." and "AQ..."
    // keys, so treating one of those as THE Google format rejected real keys.
    if (p === 'compatible' && /^(AIza|AQ\.)/.test(v)) {
      toast('that looks like a Google key — switch the provider to Google Gemini first');
      return;
    }
    localStorage.setItem(keyKey(p), v); $('#geminiKey').value = ''; refreshKeyState();
    toast('key saved for ' + p + ' (this browser only)');
  };
  $('#keyClear').onclick = () => {
    localStorage.removeItem(keyKey(getProvider())); refreshKeyState();
    toast('key cleared for ' + getProvider());
  };

  // Provider settings live beside the key, in the same browser-only storage.
  // The base URL and model are remembered because retyping a base URL every
  // session is exactly the friction that sends people back to a dead key.
  const PROV_LS = 'sf-ai-provider', BASE_LS = 'sf-ai-base';
  // The model is stored PER PROVIDER. One shared slot meant switching back to
  // Gemini carried a Groq model name with it, and Gemini's 404 for a model it
  // has never heard of is indistinguishable from a dead key.
  const modelKey = p => 'sf-ai-model-' + p;
  const getProvider = () => localStorage.getItem(PROV_LS) || 'gemini';
  const getModel = () => localStorage.getItem(modelKey(getProvider())) || '';
  function refreshProvider() {
    const p = getProvider();
    $('#aiProvider').value = p;
    $('#aiCompatFields').classList.toggle('hidden', p !== 'compatible');
    $('#aiBase').value = localStorage.getItem(BASE_LS) || '';
    $('#aiModel').value = getModel();
    $('#aiModel').placeholder = p === 'gemini'
      ? 'model (blank = server default)' : 'model, e.g. llama-3.3-70b-versatile';
    $('#aiModelHint').textContent = p === 'gemini'
      ? 'Leave blank unless "Test connection" says the default model is missing — that is a model fault, not a key fault.'
      : 'Required. Test connection lists what this key can actually use.';
    $('#geminiKey').placeholder = p === 'gemini'
      ? 'paste your key (AIza…)' : 'paste your key (blank for local Ollama)';
    // Keys are per provider now, so the saved-state line has to follow the
    // switch -- otherwise it reports the other provider's key as this one's.
    refreshKeyState();
  }
  $('#aiProvider').onchange = e => {
    localStorage.setItem(PROV_LS, e.target.value); refreshProvider();
    $('#keyTestOut').textContent = '';
  };
  $('#aiBase').onchange  = e => {
    localStorage.setItem(BASE_LS, e.target.value.trim());
    refreshSetup();   // a local Ollama base needs no key, so the nudge clears
  };
  $('#aiModel').onchange = e =>
    localStorage.setItem(modelKey(getProvider()), e.target.value.trim());
  refreshProvider();

  $('#keyTest').onclick = async () => {
    const box = $('#keyTestOut');
    box.textContent = 'testing…';
    try {
      // Prefer what is TYPED over what is saved. Testing the stored key while a
      // freshly pasted one sits in the box unsaved is how "I made a new key and
      // it still fails" happens -- the new key was never the one under test.
      const typed = $('#geminiKey').value.trim();
      const d = await post('/api/aitest', {
        provider: getProvider(), key: typed || getKey(),
        base: localStorage.getItem(BASE_LS) || '', model: getModel(),
      });
      if (typed) box.textContent = '(testing the key in the box, not the saved one) ';
      let msg = (d.ok ? '✓ ' : '✗ ') + (d.verdict || '');
      if (d.suggest && d.suggest.length) msg += '  Try: ' + d.suggest.join(', ');
      if (d.detail) msg += '  [' + d.detail + ']';
      box.textContent = msg;
    } catch (e) {
      box.textContent = '✗ ' + (e.message || e);
    }
  };

  // ---- drawer: three tabs instead of one 14-section scroll ---------------
  // The API key, the query builder and ten separate engines used to share a
  // single column, so the one panel you wanted was always somewhere in a
  // thousand pixels of something else.
  const PANE_TITLE = { build: 'Build a search', tools: 'Specialist tools', settings: 'Settings' };
  function showPane(name) {
    if (!PANE_TITLE[name]) name = 'build';
    $$('.tabpane').forEach(p => p.classList.toggle('on', p.id === 'pane-' + name));
    $$('.tab').forEach(t => t.classList.toggle('on', t.dataset.pane === name));
    $('#drawerTitle').textContent = PANE_TITLE[name];
    const body = $('.drawer-body'); if (body) body.scrollTop = 0;
  }
  $$('.tab').forEach(t => { t.onclick = () => showPane(t.dataset.pane); });

  // The drawer is `inert` while closed, so Tab cannot wander into a panel you
  // cannot see; opening moves focus in, closing hands it back to where it was.
  let drawerReturnFocus = null;
  function openDrawer(pane) {
    if (pane) showPane(pane);
    const d = $('#drawer');
    const wasOpen = !d.classList.contains('hidden');
    d.classList.remove('hidden'); d.inert = false; $('#scrim').classList.remove('hidden');
    if (!wasOpen) {
      drawerReturnFocus = document.activeElement;
      const first = d.querySelector('.tab.on') || d.querySelector('button, select, input');
      if (first) setTimeout(() => first.focus(), 0);
    }
  }
  function closeDrawer() {
    const d = $('#drawer');
    if (d.classList.contains('hidden')) return;
    d.classList.add('hidden'); d.inert = true; $('#scrim').classList.add('hidden');
    if (drawerReturnFocus && document.contains(drawerReturnFocus)) drawerReturnFocus.focus();
    drawerReturnFocus = null;
  }
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape' && !$('#drawer').classList.contains('hidden')) closeDrawer();
  });
  // Open the Tools tab with one engine already unfolded and scrolled to. Used
  // both by the toolbar and by a routed prompt that needs input we don't have.
  function openTool(id, focusSel) {
    openDrawer('tools');
    const d = $('#tool-' + id); if (!d) return;
    $$('.tool').forEach(t => { if (t !== d) t.open = false; });
    d.open = true;
    d.scrollIntoView({ block: 'start', behavior: 'smooth' });
    if (focusSel) { const f = $(focusSel); if (f) setTimeout(() => f.focus(), 220); }
  }
  $('#gear').onclick      = () => openDrawer('settings');
  $('#buildBtn').onclick  = () => openDrawer('build');
  $('#toolsBtn').onclick  = () => openDrawer('tools');
  $('#drawerClose').onclick = closeDrawer;
  $('#scrim').onclick = () => { closeDrawer(); closeSidebar(); };
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') { closeDrawer(); closeSidebar(); }
  });

  // ---- first run: say the key is needed BEFORE the question is typed -------
  // The gate used to fire only on submit, so the first thing a new visitor got
  // back from their first question was a refusal.
  function refreshSetup() {
    const local = /localhost|127\.0\.0\.1/.test(localStorage.getItem(BASE_LS) || '');
    const ready = !!getKey() || (getProvider() === 'compatible' && local);
    $('#setup').classList.toggle('hidden', ready);
    // The examples stay up even without a key: they are what tells a visitor
    // what this thing can be asked. Clicking one leaves the sentence in the box
    // and opens settings, so the key gets added to a question already written.
    $('#nl').placeholder = ready
      ? 'e.g. a woodland mansion within 300 blocks of spawn, with a village next to it'
      : 'Add an AI key to ask in words — or build the search by hand →';
  }
  $('#setupKey').onclick   = () => { openDrawer('settings'); $('#geminiKey').focus(); };
  $('#setupBuild').onclick = () => openDrawer('build');

  // ---- example prompts ----------------------------------------------------
  // A blank box with a blank placeholder answered none of "what can I ask?",
  // "how specific may I be?", or "does it know about loot?".
  const EXAMPLES = [
    'a woodland mansion within 300 blocks of spawn',
    'village right next to a mansion',
    'stronghold with 12 eyes already in the portal',
    'a village with at least 5 smiths',
    'buried treasure encased in magma',
    'diamonds within 64 blocks of spawn',
  ];
  (function buildChips() {
    const box = $('#chips');
    EXAMPLES.forEach(t => {
      const b = el('button', 'chip', t);
      b.type = 'button';
      b.onclick = () => { $('#nl').value = t; submitPrompt(); };
      box.append(b);
    });
  })();

  function aiNote(msg, isErr) {
    const n = $('#aiNote'); n.textContent = msg || ''; n.classList.toggle('err', !!isErr);
  }

  // Inverse of buildQuery: turn a query object back into the builder's condition
  // list, so an AI-generated search shows up (and can be tweaked) in settings.
  //
  // The builder cannot express everything a query can (a distance metric, a
  // biome family, a Y band, cave_below...), and this mapping used to drop
  // whole condition types: an overlap or a stronghold came back as a PLAINS
  // BIOME, and Back, Continue and Save then ran that instead. So the loaded
  // query is kept exactly as it was, and buildQuery returns it untouched until
  // the builder is actually edited (builderSnapshot differs).
  let loadedQuery = null, loadedSnap = null;
  const builderSnapshot = () => JSON.stringify({ c: conds, v: $('#version').value,
    lb: largeBiomes(), r: rank.on ? [rank.of, rank.by, rank.top] : null });
  function loadQueryIntoBuilder(query) {
    conds = (query.conditions || []).map(c => {
      if (c.eyes !== undefined) return { id: c.id, kind: 'eyes', eyes: c.eyes };
      if (c.loot) return { id: c.id, kind: 'loot', struct: c.loot.structure,
        item: c.loot.item, count: c.loot.count, within: c.within || 3000 };
      if (c.ore) {
        const o = { id: c.id, kind: 'ore', mat: c.ore, count: c.count || 1,
          within: c.within || 64, of: c.of || 'origin' };
        if (c.vein) o.vein = c.vein;
        if (c.exposed) o.exposed = true;
        if (c.deepslate || (c.y_max !== undefined && c.y_max < 0)) o.deepslate = true;
        return o;
      }
      if (c.stronghold !== undefined && c.stronghold !== null && c.stronghold !== false) {
        const named = { exposed: 0, shallow: 4 };
        const depth = c.max_depth !== undefined ? c.max_depth
                    : typeof c.stronghold === 'number' ? c.stronghold
                    : named[c.stronghold] !== undefined ? named[c.stronghold] : 60;
        return { id: c.id, kind: 'stronghold', depth, land: c.land !== false,
                 within: c.within || 3000, of: c.of || 'origin' };
      }
      if (Array.isArray(c.overlap)) return { id: c.id, kind: 'overlap',
        a: c.overlap[0], b: c.overlap[1], pad: c.pad || 0,
        within: c.within || 2000, of: c.of || 'origin',
        minRoofY: c.min_roof_y !== undefined ? c.min_roof_y : null,
        maxDepth: c.max_depth !== undefined ? c.max_depth : null };
      if (c.slime !== undefined) return { id: c.id, kind: 'slime', count: c.slime,
        within: c.within || 128, of: c.of || 'origin' };
      if (c.biome_area) return { id: c.id, kind: 'area', name: c.biome_area,
        pct: c.pct || 50, within: c.within || 800, of: c.of || 'origin', precision: c.precision || 'fine' };
      if (c.island) return { id: c.id, kind: 'island', pct: c.pct || 75,
        within: c.within || 256, of: c.of || 'spawn' };
      if (c.height !== undefined || c.relief !== undefined)
        return { id: c.id, kind: 'height',
          height: c.height != null ? c.height : -64, relief: c.relief || 0,
          exact: !!c.exact,
          within: c.within || 300, of: c.of || 'origin', precision: 'fine' };
      if (c.structure) {
        const o = { id: c.id, kind: 'structure', name: c.structure,
          within: c.within || 400, of: c.of || 'origin', precision: 'fine' };
        ['surface', 'giant', 'abandoned', 'basement', 'ship'].forEach(k => { if (c[k]) o[k] = true; });
        if (c.not) o.not = true;
        if (c.count) o.count = c.count;
        if (c.spread) o.spread = c.spread;
        if (c.structure === 'geode') {
          if (c.size) o.gsize = c.size;
          if (c.cracked === false) o.sealed = 'sealed';
          if (c.cracked === true) o.sealed = 'cracked';
        }
        return o;
      }
      const b = { id: c.id, kind: 'biome', name: c.biome || 'plains',
        within: c.within || 400, of: c.of || 'origin', precision: c.precision || 'fine' };
      if (c.not) b.not = true;
      return b;
    });
    nextId = conds.reduce((m, c) =>
      Math.max(m, parseInt(String(c.id).replace(/\D/g, '')) || 0), conds.length) + 1;
    $('#largeBiomes').checked = !!query.large_biomes;
    rank.on = !!query.rank;
    if (query.rank) { rank.of = query.rank.of || ''; rank.by = query.rank.by || 'auto';
                      rank.top = query.rank.top || 10; }
    syncEdition();
    render();
    loadedQuery = JSON.parse(JSON.stringify(query));
    delete loadedQuery.budget; delete loadedQuery.tool;
    loadedSnap = builderSnapshot();
  }

  async function submitPrompt() {
    const text = $('#nl').value.trim();
    if (!text) return;
    const key = getKey();
    const prov = getProvider();
    // A local Ollama has no key and never will, so the gate asks for one only
    // where one is actually required. It still catches the common case: a
    // remote provider selected with nothing filled in.
    if (!key && !(prov === 'compatible' && /localhost|127\.0\.0\.1/.test(localStorage.getItem(BASE_LS) || ''))) {
      aiNote(prov === 'gemini'
        ? 'Add a free Gemini key to search by description — opening settings…'
        : 'Add a key for your provider, or point the base URL at a local model — opening settings…', true);
      openDrawer('settings'); $('#geminiKey').focus(); return;
    }
    if (prov === 'compatible' && !(localStorage.getItem(BASE_LS) || '').trim()) {
      aiNote('Set the API base URL for your provider — opening settings…', true);
      openDrawer('settings'); $('#aiBase').focus(); return;
    }
    // Asking something new replaces what came before, the way a chat does:
    // stop whatever is still running, empty the results, and clear the box so
    // the next question starts from a blank prompt rather than the last one.
    const token = beginRun();
    addHistory(text);
    $('#nl').value = ''; $('#nl').style.height = 'auto';
    out.innerHTML = ''; stopScroll();
    aiNote('thinking…'); setBusyUI(true);
    try {
      const d = await post('/api/gemini',
                           // The model is per-provider, so it is always safe to
                           // send: Gemini can never receive a Groq model name.
                           // Blank means "server default".
                           Object.assign(
                             { text, version: $('#version').value, key,
                               provider: prov, model: getModel() },
                             prov === 'compatible'
                               ? { base: localStorage.getItem(BASE_LS) || '' }
                               : {}),
                           runCtl && runCtl.signal);
      if (token !== runToken) return;
      aiNote(d.notes ? d.notes : '');
      // A prompt that needs a different engine now RUNS it, rather than being
      // told which panel to click. The conditions still populate the builder so
      // what ran is visible and tweakable.
      // Accept it in either place: the server puts it on the query, but a
      // model that emits it at the top level should not be silently ignored.
      const t = (d.query && d.query.tool) || d.tool;
      if (t && t.name) {
        delete d.query.tool;
        try { loadQueryIntoBuilder(d.query); } catch (e) {}
        setBusyUI(false); endRun();
        await routeTool(t, token);
        return;
      }
      // Defensive: a malformed tool field must not reach the C engine, which
      // would reject the query outright over a key it never asked for.
      if (d.query) delete d.query.tool;
      if (d.query) rememberQuery(text, d.query, d.notes);
      // The failure this guards against actually happened: a coal-ore casing
      // request returned a note saying it had been routed to the casing tool,
      // and then ran a plain buried-treasure search. Prose claiming a route
      // while the ordinary search runs is worse than no route at all, so say so.
      if (/routed?/i.test(d.notes || '')) {
        aiNote((d.notes || '') + '  — but no tool was actually selected, so this '
             + 'is the ORDINARY search, not that tool. Use the sidebar panel directly.', true);
      }
      await runSearch(d.query, token);
    } catch (e) {
      if (token !== runToken) return;
      setBusyUI(false); endRun();
      if (!isAbort(e)) aiNote(e.message, true);
    }
  }
  $('#promptForm').addEventListener('submit', e => { e.preventDefault(); submitPrompt(); });

  // Enter submits, Shift+Enter makes a newline; textarea auto-grows.
  const nlBox = $('#nl');
  nlBox.addEventListener('input', () => {
    nlBox.style.height = 'auto'; nlBox.style.height = Math.min(200, nlBox.scrollHeight) + 'px';
  });
  nlBox.addEventListener('keydown', e => {
    // isComposing: Enter also CONFIRMS a word in Japanese/Chinese/Korean input,
    // and submitting then sent half a sentence. And the ➜ button is disabled
    // while a run is busy; Enter has to respect that too.
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && e.keyCode !== 229) {
      e.preventDefault();
      if (!$('#go').disabled) submitPrompt();
    }
  });

  // ---- bedrock glitched outpost -----------------------------------------
  $('#boBtn').onclick = async () => {
    const seeds = $('#boSeeds').value.trim() || '12345';
    const min_drop = Math.max(4, Math.min(120, +$('#boDrop').value || 20));
    const verify_top = Math.max(1, Math.min(10, +$('#boVerify').value || 2));
    busy('running a real Bedrock Dedicated Server and measuring cobblestone under each outpost. Slow.');
    try {
      const d = await post('/api/bedrockoutpost', { seeds, min_drop, verify_top });
      out.innerHTML = '';
      const tall = d.hits.filter(h => (h.cobblestone_height || 0) > 0);
      out.append(statsCard('Bedrock glitched outpost · real BDS', [
        ['candidates probed', String(d.hits.length)],
        ['with a cobblestone column', String(tall.length), tall.length ? 'good' : 'warn'],
        ['tallest', tall.length ? (tall[0].cobblestone_height + ' blocks') : '—'],
      ]));
      if (!tall.length) {
        out.append(el('div', 'note warn',
          'No cobblestone foundation among the candidates probed — not found in what was '
          + 'scanned, rather than none existing. Lower the minimum drop or add seeds.'));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Outposts on a cobblestone column'));
        tall.forEach(h => {
          const box = el('div', 'seed'), n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(h.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = h.seed, 800); };
          n.append(num, el('span', 'dim', h.cobblestone_height + ' blocks of cobblestone'));
          box.append(n);
          const t = el('table'), tr = el('tr');
          tr.append(idCell('structure', 'pillager_outpost', 'Outpost'),
                    el('td', null, `x=${h.x}  z=${h.z}`),
                    el('td', 'd', `drop ${h.predicted_drop}`));
          t.append(tr); box.append(t); c.append(box);
        });
        out.append(c);
      }
      if (d.note) out.append(el('div', 'note', d.note));
    } catch (e) { fail(e.message); }
  };

  // ---- piece-exact structure overlap -------------------------------------
  $('#csBtn').onclick = async () => {
    const seed = $('#csSeed').value.trim();
    if (!seed) { fail('need a seed — this reads a world you already have'); return; }
    const body = { seed, version: $('#version').value,
                   radius: +$('#csRadius').value || 3000,
                   item: $('#csItem').value.trim(),
                   min: +$('#csMin').value || 1,
                   ench: $('#csEnch').value.trim(),
                   ench_min: +$('#csEnchMin').value || 1 };
    busy('reading every lootable chest in range of that seed.');
    try {
      const d = await post('/api/coolstuff', body);
      out.innerHTML = '';
      out.append(statsCard('Cool stuff in this seed', [
        ['chests read', String(d.chests ?? 0)],
        ['worth the trip', String(d.finds.length), d.finds.length ? 'good' : 'warn'],
        ['looking for', body.ench ? `${pretty(body.ench)} ${body.ench_min}+`
                        : body.item ? `${body.min}+ ${pretty(body.item)}`
                        : 'anything notable'],
      ]));
      if (!d.finds.length) {
        out.append(el('div', 'note warn',
          `Nothing cleared the bar among the ${d.chests ?? 0} chests within ${body.radius} `
          + `blocks. That is about this radius, not about the seed — widen it, or name a `
          + `different item.`));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Worth the trip'));
        d.finds.forEach(f => {
          const box = el('div', 'seed');
          const n = el('div', 'n');
          n.append(el('span', 'num', pretty(f.structure)),
                   el('span', 'dim', f.why));
          box.append(n);
          const t = el('table'), tr = el('tr');
          tr.append(idCell('structure', f.structure, pretty(f.structure)),
                    el('td', null, `x=${f.x}  z=${f.z}`),
                    el('td', 'd', `/tp ${f.x} ~ ${f.z}`));
          t.append(tr); box.append(t);
          if (f.chest) {
            const fl = el('div', 'dim');
            fl.style.cssText = 'font-size:12px;margin-top:4px';
            fl.textContent = f.chest;
            box.append(fl);
          }
          c.append(box);
        });
        out.append(c);
      }
      // A structure that could not be located is a HOLE in the scan. Reporting
      // it as "nothing there" is the failure this project keeps rediscovering.
      if (d.skipped && d.skipped.length)
        out.append(el('div', 'note warn',
          'NOT SCANNED: ' + d.skipped.join(', ') + ' — these were not searched at all, '
          + 'which is not the same as having nothing in them.'));
      if (d.note) out.append(el('div', 'note', d.note));
    } catch (e) { fail(e.message); }
  };

  $('#svBtn').onclick = async () => {
    const body = { min: +$('#svMin').value || 2,
                   radius: +$('#svRadius').value || 3,
                   seeds: +$('#svSeeds').value || 20,
                   version: $('#version').value };
    busy('enumerating real mineshaft pieces around each village — this runs the game\'s own generator.');
    try {
      const d = await post('/api/spidervillage', body);
      out.innerHTML = '';
      out.append(statsCard('Cave spiders by a village', [
        ['villages checked', String(d.checked ?? 0)],
        ['matched', String(d.hits.length), d.hits.length ? 'good' : 'warn'],
        ['wanted', `${body.min}+ within ${body.radius} chunks`],
      ]));
      if (!d.hits.length) {
        // "None in what was scanned" is not "none exist", and the difference is
        // the whole reason this panel reports how many it checked.
        out.append(el('div', 'note warn',
          `No village had ${body.min}+ spider corridors within ${body.radius} chunks `
          + `among the ${d.checked ?? 0} checked. Scan more seeds, widen the radius, `
          + `or ask for fewer — this is a statement about what was scanned.`));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Spider villages — click a seed to copy'));
        d.hits.forEach(h => {
          const box = el('div', 'seed');
          const n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(h.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = h.seed, 800); };
          n.append(num, el('span', 'dim', `${h.spawners} spawner corridor(s)`));
          box.append(n);
          const t = el('table'), tr = el('tr');
          tr.append(idCell('structure', 'village', 'Village'),
                    el('td', null, `x=${h.x}  z=${h.z}`),
                    el('td', 'd', `/tp ${h.x} 90 ${h.z}`));
          t.append(tr); box.append(t);
          (h.boxes || []).forEach(b => {
            const fl = el('div', 'dim');
            fl.style.cssText = 'font-size:12px;margin-top:3px';
            fl.textContent = `${b.variant}  ${b.min.join(',')} .. ${b.max.join(',')}`;
            box.append(fl);
          });
          c.append(box);
        });
        out.append(c);
      }
      if (d.note) out.append(el('div', 'note', d.note));
    } catch (e) { fail(e.message); }
  };

  $('#flBtn').onclick = async () => {
    const seed = $('#flSeed').value.trim();
    if (!seed) { fail('need a seed'); return; }
    const body = { seed, x: +$('#flX').value, z: +$('#flZ').value,
                   gap: +$('#flGap').value || 24 };
    busy('reading the real terrain column out of the game\'s own generator.');
    try {
      const d = await post('/api/confirmfloating', body);
      out.innerHTML = '';
      out.append(statsCard('Floating island check', [
        ['verdict', d.unknown ? 'UNKNOWN' : d.floating ? 'FLOATING' : 'not floating',
         d.floating ? 'good' : d.unknown ? 'warn' : ''],
        ['island top', d.top == null ? '—' : 'y=' + d.top],
        ['underside', d.cap == null ? '—' : 'y=' + d.cap],
        ['void beneath', (d.void || 0) + ' blocks'],
        ['sides dropping away', (d.lower || 0) + '/8'],
      ]));
      if (d.unknown) {
        // An UNKNOWN must never render as a "no". A confirmer that guesses
        // certifies islands that are not there, which is the one failure this
        // panel exists to prevent.
        out.append(el('div', 'note warn',
          'UNKNOWN — the backend could not read that column. This is NOT a finding '
          + 'that the spot is ordinary ground.'));
      } else if (d.floating) {
        out.append(el('div', 'note',
          `Solid cap at y=${d.cap} with ${d.void} blocks of nothing under it, and `
          + `${d.lower} of 8 surrounding columns top out below the underside.`));
      } else {
        out.append(el('div', 'note',
          (d.void || 0) < 8
            ? `Only ${d.void || 0} blocks of void under the cap — that is a dip, not an island.`
            : `The void is real but only ${d.lower}/8 sides drop away, so this is a `
              + `cave roof or an overhang rather than an island.`));
      }
      if (d.ring && d.ring.length) {
        const pre = el('pre', 'mono');
        pre.textContent = d.ring.join('\n');
        out.append(pre);
      }
    } catch (e) { fail(e.message); }
  };

  $('#soBtn').onclick = async () => {
    const seed = $('#soSeed').value.trim();
    if (!seed) { fail('need a seed'); return; }
    const body = { seed, a: $('#soA').value.trim(), ax: +$('#soAx').value, az: +$('#soAz').value,
                   b: $('#soB').value.trim(), bx: +$('#soBx').value, bz: +$('#soBz').value };
    busy('assembling both structures and intersecting their real piece boxes.');
    try {
      const d = await post('/api/structoverlap', body);
      out.innerHTML = '';
      const v = d.verdict;
      out.append(statsCard('Piece-exact overlap', [
        ['verdict', v === 'overlap' ? 'THEY OVERLAP' : v === 'separate' ? 'separate' : 'UNKNOWN',
         v === 'overlap' ? 'good' : v === 'unknown' ? 'warn' : ''],
        ['intersecting piece pairs', String(d.pairs || 0)],
        ['shared volume', (d.blocks || 0).toLocaleString() + ' blocks'],
      ]));
      if (v === 'unknown') {
        // Never render this as a negative -- it is the engine saying it cannot
        // assemble one of the structures, not that it checked and found nothing.
        out.append(el('div', 'note warn',
          'UNKNOWN — this engine cannot assemble one of those structures, so whether '
          + 'they overlap is not decidable here. That is NOT a finding that they are separate.'));
      } else if (v === 'separate' && d.gap != null) {
        out.append(el('div', 'note',
          `No piece of one meets any piece of the other; nearest pieces are ${d.gap} blocks apart.`));
      }
      if (d.note) out.append(el('div', 'note', d.note));
      const pre = el('div', 'card'); pre.append(el('h3', null, 'Raw'));
      const p = el('pre'); p.textContent = d.raw || ''; pre.append(p); out.append(pre);
    } catch (e) { fail(e.message); }
  };

  // ---- prompt -> the engine that can actually answer it ------------------
  // The prompt box drives one engine by default (cubiomes). Some questions need
  // another, and being told to go click a panel is not an answer -- so a routed
  // prompt fills in that panel's controls and presses its button.
  const TOOL_PANEL = {
    casing: 'casing', village_building: 'village', exposed_treasure: 'exposed',
    fortress_overlap: 'fortress', bedrock_outpost: 'bedrock', cool_stuff: 'cool',
    spider_village: 'spider', floating: 'floating', struct_overlap: 'overlap',
    hunt: 'hunt',
  };
  async function routeTool(t, token) {
    const set = (id, v) => { const e = $(id); if (e && v != null) e.value = v; };
    // Unfold the matching panel in the drawer without opening the drawer over
    // the results: whoever wants to see or adjust what ran finds it already
    // expanded rather than having to work out which of ten it was.
    const panel = TOOL_PANEL[t.name];
    if (panel) { const d = $('#tool-' + panel); if (d) d.open = true; }
    switch (t.name) {
      case 'casing':
        set('#tcCasing', String(t.casing || 'magma_block').replace(/^minecraft:/, ''));
        set('#tcRadius', t.radius || 10000);
        set('#tcLimit', t.want || 1);
        // "Keep looking", not the single batch: a casing worth asking for by
        // name is rare enough that one batch is the wrong tool.
        $('#tcKeep').click();
        return;
      case 'village_building':
        set('#vsBuilding', t.building || 'smith');
        set('#vsMin', t.min || 5);
        set('#vsSeeds', t.seeds || 200);
        $('#vsBtn').click();
        return;
      case 'exposed_treasure':
        set('#etLimit', t.want || 3);
        $('#etBtn').click();
        return;
      case 'fortress_overlap':
        set('#foSeeds', t.seeds || 200000);
        $('#foBtn').click();
        return;
      case 'bedrock_outpost':
        // Bedrock requests must NOT fall back to the Java search: its seeds do
        // not transfer, so an approximate Java answer is not an approximate
        // answer at all.
        if (t.seed_list) set('#boSeeds', t.seed_list);
        set('#boDrop', t.min_drop || 20);
        $('#boBtn').click();
        return;
      case 'cool_stuff':
        if (t.seed) set('#csSeed', t.seed);
        if (t.item) set('#csItem', t.item);
        if (t.min) set('#csMin', t.min);
        if (t.ench) set('#csEnch', t.ench);
        if (t.ench_min) set('#csEnchMin', t.ench_min);
        if (t.seed) { $('#csBtn').click(); return; }
        // Missing one field is not a reason to hand back directions. Open the
        // tool, fill in what the model did give, and put the cursor in the gap.
        aiNote('This reads a seed you already have — which seed? I have opened the '
             + 'panel with everything else filled in.', true);
        openTool('cool', '#csSeed');
        return;
      case 'spider_village':
        if (t.min) set('#svMin', t.min);
        if (t.radius) set('#svRadius', t.radius);
        $('#svBtn').click();
        return;
      case 'floating':
        if (t.seed) set('#flSeed', t.seed);
        if (t.x != null) set('#flX', t.x);
        if (t.z != null) set('#flZ', t.z);
        aiNote('This confirms one spot rather than searching — give it the seed and '
             + 'the position, then press Check floating.', true);
        openTool('floating', t.seed ? '#flX' : '#flSeed');
        return;
      case 'struct_overlap':
        if (t.a) set('#soA', t.a);
        if (t.b) set('#soB', t.b);
        if (t.seed) set('#soSeed', t.seed);
        aiNote('This needs a seed and both structure positions — the rest is filled '
             + 'in, then press Check overlap.', true);
        openTool('overlap', '#soSeed');
        return;
      case 'hunt':
        set('#huWant', t.want || 1);
        $('#huBtn').click();
        return;
      default:
        aiNote(`I routed this to "${t.name}", which is not a tool I have. `
             + `Running the ordinary search instead.`, true);
        await runSearch({ version: $('#version').value, conditions: [] }, token);
    }
  }

  // ---- village smith search (real worldgen, tier-2 backend) --------------
  function prettyBuildings(s) {
    return String(s).replace(/[{}]/g, '').split(', ').filter(Boolean).map(x => {
      const [k, v] = x.split('=');
      const name = k.replace(/^(taiga|plains|desert|savanna|snowy)_/, '').replace(/_house$/, '');
      return pretty(name) + ' ×' + v;
    }).join(', ');
  }
  $('#vsBtn').onclick = async () => {
    const mn = Math.max(1, Math.min(12, +$('#vsMin').value || 5));
    const seeds = Math.max(1, Math.min(3000, +$('#vsSeeds').value || 200));
    const building = $('#vsBuilding').value;
    const label = building === 'smith' ? 'smiths' : pretty(building) + 's';
    busy(`running Minecraft's real world generator — scanning ${seeds.toLocaleString()} seeds for a village with ${mn}+ ${label}. This is slow.`);
    try {
      const d = await post('/api/villagesmiths', { min: mn, building, seeds, radius: 1200, workers: 4, limit: 20 });
      out.innerHTML = '';
      out.append(statsCard('Village building search · real MC 1.16.5 worldgen', [
        ['seeds scanned', (d.summary?.seeds_scanned ?? seeds).toLocaleString()],
        ['villages found', String(d.hits.length), d.hits.length ? 'good' : 'warn'],
        [label, '≥ ' + mn],
      ]));
      if (!d.hits.length) {
        out.append(el('div', 'note warn',
          `No village with ${mn}+ ${label} in the first ${seeds.toLocaleString()} seeds — lower the threshold or scan more.`));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Villages — click a seed to copy'));
        d.hits.forEach(h => {
          const box = el('div', 'seed');
          const n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(h.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = h.seed, 800); };
          n.append(num, el('span', 'dim', h.smiths + ' ' + label)); box.append(n);
          const t = el('table');
          const tr = el('tr');
          tr.append(idCell('structure', 'village', 'Village'),
                    el('td', null, `x=${h.x}  z=${h.z}`), el('td', 'd', pretty(h.biome)));
          t.append(tr);
          const tr2 = el('tr');
          tr2.append(el('td', 'id', ''), el('td', null, prettyBuildings(h.buildings)), el('td', 'd', ''));
          t.append(tr2);
          box.append(t); c.append(box);
        });
        out.append(c);
      }
    } catch (e) { fail(e.message); }
  };

  // ---- treasure casing (tier-2 OreGen: decoration, so it can see ore) -----
  $('#tcBtn').onclick = async () => {
    const casing = $('#tcCasing').value;
    const limit = Math.max(1, Math.min(10, +$('#tcLimit').value || 1));
    const range = Math.max(100000, Math.min(200000000, +$('#tcRange').value || 3000000));
    busy(`scanning for a buried treasure encased in ${pretty(casing)} — running real ` +
         `worldgen with decoration on every candidate. Rare ones take a while.`);
    try {
      const radius = Math.max(1000, Math.min(60000, +$('#tcRadius').value || 10000));
      const d = await post('/api/casingtreasure', { casing, limit, range, radius, workers: 4 });
      out.innerHTML = '';
      out.append(statsCard('Buried treasure casing · real MC 1.21.1 worldgen', [
        ['chests checked', String(d.summary?.checked ?? 0)],
        ['found', String(d.hits.length), d.hits.length ? 'good' : 'warn'],
        ['encased in', pretty(casing)],
      ]));
      if (!d.hits.length) {
        // Never "none exist". This is a bounded scan of a rare thing, and the
        // difference matters most exactly when the result is empty.
        out.append(el('div', 'note warn',
          `No ${pretty(casing)} casing among the ${d.summary?.checked ?? 0} chests scanned. ` +
          `That means not found in what was scanned — not that none exists. Scan more seeds.`));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Encased treasures — click a seed to copy'));
        d.hits.forEach(h => {
          const box = el('div', 'seed');
          const n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(h.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = h.seed, 800); };
          n.append(num, el('span', 'dim',
            pretty(casing) + (h.faces ? ` \u00d7${h.faces} of 6 faces` : '')));
          box.append(n);
          const t = el('table'), tr = el('tr');
          tr.append(idCell('structure', 'buried_treasure', 'Buried treasure'),
                    el('td', null, `x=${h.x}  y=${h.y ?? '?'}  z=${h.z}`),
                    el('td', 'd', `/tp ${h.x} ${h.y ?? 64} ${h.z}`));
          t.append(tr); box.append(t);
          // The six faces as they are AFTER decoration -- what a player digs
          // up, not what the structure wrote. Those differ routinely: the
          // treasure is placed in SURFACE_STRUCTURES and ore in
          // UNDERGROUND_ORES, so a later blob can land on a finished chest.
          if (h.all_faces && h.all_faces.length === 6) {
            const order = ['west', 'east', 'down', 'up', 'north', 'south'];
            const fl = el('div', 'dim');
            fl.style.cssText = 'font-size:12px;margin-top:4px';
            fl.textContent = h.all_faces
              .map((b, i) => `${order[i]} ${pretty(String(b).replace('minecraft:', ''))}`)
              .join(' · ');
            box.append(fl);
          }
          // Every hit is a lead until a real server says otherwise, so the way
          // to settle it sits on the hit itself rather than in a footnote.
          const cb = el('button', null, 'confirm on real server');
          cb.onclick = () => confirmCasing(cb, h.seed, h.x, h.z, casing);
          box.append(cb);
          c.append(box);
        });
        out.append(c);
      }
      if (d.confirm) out.append(el('div', 'note', d.confirm));
    } catch (e) { fail(e.message); }
  };

  // A rare casing is exactly the case a single batch cannot answer: at ~0.015%
  // one pass is nowhere near enough, and without an advancing offset a second
  // pass would rescan the first. So the casing panel can run open-ended too.
  // A hunt renders into its OWN view inside #out. The pollers used to wipe
  // #out every few seconds whatever was on screen -- open a map mid-hunt and
  // it vanished on the next tick. Now a poll redraws only while its view is
  // still showing, and keeps polling regardless (the server stops hunts that
  // nobody checks on). A lost server, or one that restarted and no longer
  // knows the hunt, is said out loud instead of reading as "nothing yet".
  function huntPoller(job, view, every, draw, onEnd) {
    let misses = 0;
    const timer = setInterval(async () => {
      let s;
      try { s = await post('/api/hunt/status', { job }); misses = 0; }
      catch (e) {
        if (++misses === 3 && out.contains(view))
          view.prepend(el('div', 'note bad', 'Lost contact with the server — is it still running? Retrying…'));
        return;
      }
      if (!s.known) {
        if (out.contains(view)) view.prepend(el('div', 'note bad',
          'The server no longer knows this hunt — it was probably restarted. Start it again.'));
        clearInterval(timer); onEnd(); return;
      }
      if (out.contains(view)) { view.innerHTML = ''; draw(s); }
      if (!s.running) { clearInterval(timer); onEnd(); }
    }, every);
    return timer;
  }

  let tcJob = null, tcPoll = null;
  function tcStopUI() {
    clearInterval(tcPoll); tcPoll = null; tcJob = null;
    $('#tcStop').disabled = true; $('#tcKeep').disabled = false; $('#tcBtn').disabled = false;
  }
  $('#tcKeep').onclick = async () => {
    const casing = $('#tcCasing').value;
    const want = Math.max(1, Math.min(10, +$('#tcLimit').value || 1));
    const batch = Math.max(100000, Math.min(50000000, +$('#tcRange').value || 3000000));
    tcJob = 'casing-' + Date.now();
    const job = tcJob;
    $('#tcKeep').disabled = true; $('#tcBtn').disabled = true; $('#tcStop').disabled = false;
    busy(`hunting a ${pretty(casing)} casing across batch after batch of fresh seeds. `
       + `At ~0.015% for the rare ones this is the only way it can succeed.`);
    try {
      const tcRad = Math.max(1000, Math.min(60000, +$('#tcRadius').value || 10000));
      await post('/api/hunt', { job, casing, batch, want, secs: 600, radius: tcRad,
                                query: { version: $('#version').value, conditions: [] } });
    } catch (e) { fail(e.message); tcStopUI(); return; }
    const view = el('div', null, 'started — first report in a few seconds…');
    out.innerHTML = ''; out.append(view);
    tcPoll = huntPoller(job, view, 4000, s => {
      view.append(statsCard('Casing hunt · ' + pretty(casing), [
        ['batches', String(s.batches || 0)],
        ['seeds scanned', (s.scanned || 0).toLocaleString()],
        ['candidates', String(s.candidates || 0)],
        ['server said no', String(s.rejected || 0)],
        ['could not check', String(s.unknown || 0), (s.unknown || 0) ? 'warn' : ''],
        ['confirmed', String((s.hits || []).length), (s.hits || []).length ? 'good' : ''],
      ]));
      if (s.error) view.append(el('div', 'note warn', s.error));
      if ((s.hits || []).length) {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Encased treasures'));
        s.hits.forEach(h => {
          const box = el('div', 'seed'), n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => copySeed(num, String(h.seed));
          n.append(num, el('span', 'dim', `x=${h.x}  y=${h.y ?? '?'}  z=${h.z}`));
          const tp = el('button', 'tiny', '/tp'); tp.type = 'button';
          tp.onclick = () => copyText(tp, tpCommand(h.x, h.z, 'overworld', h.y));
          n.append(tp);
          box.append(n);
          const cb = el('button', null, 'confirm on real server');
          cb.onclick = () => confirmCasing(cb, h.seed, h.x, h.z, casing);
          box.append(cb); c.append(box);
        });
        view.append(c);
      } else if (!s.error) {
        view.append(el('div', 'note',
          `Nothing yet after ${(s.scanned || 0).toLocaleString()} seeds. Each batch covers ` +
          `seeds the earlier ones did not, so this is progress rather than a loop.`));
      }
    }, () => { if (tcJob === job) tcStopUI(); });
  };
  $('#tcStop').onclick = async () => {
    if (!tcJob) return;
    try { await post('/api/hunt/stop', { job: tcJob }); } catch (e) {}
    tcStopUI();
  };

  // ---- open-ended hunt ---------------------------------------------------
  // The batch search always restarted its seed walk from the same place, so
  // "search again" re-covered the same ground. This advances the offset per
  // batch, which is the difference between "not in the first N" and "keep going".
  let huntJob = null, huntPoll = null;
  function huntStopUI() {
    clearInterval(huntPoll); huntPoll = null; huntJob = null;
    $('#huStop').disabled = true; $('#huBtn').disabled = false;
  }
  $('#huBtn').onclick = async () => {
    if (!guard()) return;
    const query = buildQuery();
    const batch = Math.max(10000, Math.min(50000000, +$('#huBatch').value || 2000000));
    const want = Math.max(1, Math.min(50, +$('#huWant').value || 1));
    huntJob = 'hunt-' + Date.now();
    const job = huntJob;
    $('#huBtn').disabled = true; $('#huStop').disabled = false;
    busy('searching fresh seeds batch after batch — this keeps going until it finds what you asked for.');
    try {
      await post('/api/hunt', { query, job, batch, want, secs: 60,
                                threads: +$('#threads').value || 16 });
    } catch (e) { fail(e.message); huntStopUI(); return; }
    shownQuery = query;
    const view = el('div', null, 'started — first report in a few seconds…');
    out.innerHTML = ''; out.append(understoodCard(query), view);
    huntPoll = huntPoller(job, view, 3000, s => {
      view.append(statsCard('Open-ended search', [
        ['batches', String(s.batches || 0)],
        ['seeds scanned', (s.scanned || 0).toLocaleString()],
        ['found', String((s.hits || []).length), (s.hits || []).length ? 'good' : ''],
      ]));
      if (s.error) view.append(el('div', 'note warn', s.error));
      if ((s.hits || []).length) {
        // The same rich rows as a search -- coordinates, map links, /tp --
        // rather than bare seed numbers.
        view.append(seedsCard(s.hits, `Found: ${s.hits.length}`));
      } else if (!s.error) {
        view.append(el('div', 'note',
          `Nothing yet after ${(s.scanned || 0).toLocaleString()} seeds. Every batch ` +
          `covers seeds the earlier ones did not, so this is progress, not a loop.`));
      }
    }, () => { if (huntJob === job) huntStopUI(); });
  };
  $('#huStop').onclick = async () => {
    if (!huntJob) return;
    try { await post('/api/hunt/stop', { job: huntJob }); } catch (e) {}
    huntStopUI();
  };

  // ---- confirm a casing lead on a real server ----------------------------
  // The fast path agrees with a real server on 88% of chests, so its hits are
  // leads. Without this button that caveat is only a disclaimer.
  async function confirmCasing(btn, seed, x, z, expect) {
    btn.disabled = true; btn.textContent = 'confirming…';
    try {
      const d = await post('/api/confirmcasing', { seed, x, z, expect });
      const row = btn.closest('.seed');
      const note = el('div', 'note ' + (d.confirmed ? 'good' : 'warn'),
        d.confirmed
          ? `Confirmed on a real server: ${pretty(d.casing || expect)} on ${d.agree} of 5 faces.`
          : (d.checked
              ? `Not confirmed — the server says ${pretty(d.casing || 'something else')}. `
                + `This is the 12% the fast path gets wrong, which is why it shortlists.`
              : `Could not be checked: ${(d.error || 'unknown').slice(0, 160)}`));
      row.append(note);
      btn.textContent = d.confirmed ? 'confirmed' : 'not confirmed';
    } catch (e) {
      btn.disabled = false; btn.textContent = 'confirm';
      fail(e.message);
    }
  }

  // ---- fortress interpenetration ----------------------------------------
  $('#foBtn').onclick = async () => {
    const seed = $('#foSeed').value.trim();
    const seeds = Math.max(1000, Math.min(5000000, +$('#foSeeds').value || 200000));
    busy(seed ? `measuring how far the fortresses in ${seed} grow through each other`
              : `building every fortress in ${seeds.toLocaleString()} seeds and ranking them by real piece overlap`);
    try {
      const d = await post('/api/fortressoverlap', seed ? { seed } : { seeds });
      out.innerHTML = '';
      if (seed) {
        out.append(statsCard('Fortress interpenetration · seed ' + seed, [
          ['fortresses', String(d.summary?.fortresses ?? 0)],
          ['intersecting piece pairs', String(d.summary?.pairs ?? 0),
           (d.summary?.pairs || 0) > 100 ? 'good' : ''],
          ['shared blocks', (d.summary?.blocks ?? 0).toLocaleString()],
        ]));
        if (d.pairs.length) {
          const c = el('div', 'card');
          c.append(el('h3', null, 'Which pairs overlap'));
          const t = el('table');
          d.pairs.forEach(p => {
            const tr = el('tr');
            tr.append(el('td', 'id', `#${p.a} × #${p.b}`),
                      el('td', null, `${p.pairs} piece pairs`),
                      el('td', 'd', `${p.blocks.toLocaleString()} blocks`));
            t.append(tr);
          });
          c.append(t); out.append(c);
        }
      } else {
        out.append(statsCard('Fortress overlap · ranked', [
          ['seeds scanned', seeds.toLocaleString()],
          ['ranked', String(d.ranked.length), d.ranked.length ? 'good' : 'warn'],
        ]));
        const c = el('div', 'card');
        c.append(el('h3', null, 'Best overlap — click a seed to copy'));
        d.ranked.slice().reverse().forEach(r => {
          const box = el('div', 'seed'), n = el('div', 'n');
          const num = el('span', 'num', String(r.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(r.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = r.seed, 800); };
          n.append(num, el('span', 'dim',
            `${r.pairs} piece pairs · ${r.blocks.toLocaleString()} blocks · ${r.fortresses} fortresses`));
          box.append(n); c.append(box);
        });
        out.append(c);
      }
      if (!d.pairs.length && !d.ranked.length) {
        out.append(el('div', 'note warn', 'No overlap found in what was scanned.'));
      }
    } catch (e) { fail(e.message); }
  };

  // ---- exposed treasure --------------------------------------------------
  $('#etBtn').onclick = async () => {
    const range = Math.max(100000, Math.min(200000000, +$('#etRange').value || 3000000));
    const limit = Math.max(1, Math.min(20, +$('#etLimit').value || 3));
    busy('looking for a buried treasure chest sitting in open air — real worldgen on every candidate.');
    try {
      const d = await post('/api/exposedtreasure', { range, limit });
      out.innerHTML = '';
      out.append(statsCard('Exposed buried treasure · real MC 1.21.1 worldgen', [
        ['chests checked', String(d.summary?.checked ?? 0)],
        ['exposed', String(d.hits.length), d.hits.length ? 'good' : 'warn'],
      ]));
      if (!d.hits.length) {
        out.append(el('div', 'note warn',
          `None among the ${d.summary?.checked ?? 0} chests scanned — not found in what was ` +
          `scanned, rather than none existing. Scan more seeds.`));
      } else {
        const c = el('div', 'card');
        c.append(el('h3', null, 'Exposed treasures — click a seed to copy'));
        d.hits.forEach(h => {
          const box = el('div', 'seed'), n = el('div', 'n');
          const num = el('span', 'num', String(h.seed));
          num.title = 'copy';
          num.onclick = () => { navigator.clipboard?.writeText(String(h.seed));
            num.textContent = 'copied'; setTimeout(() => num.textContent = h.seed, 800); };
          n.append(num, el('span', 'dim', `x=${h.x}  z=${h.z}`));
          box.append(n); c.append(box);
        });
        out.append(c);
      }
    } catch (e) { fail(e.message); }
  };

  // ---- start -------------------------------------------------------------
  async function loadAssets() {
    try {
      const r = await fetch('/api/assets');
      if (r.ok) assets = await r.json();
      if (assets.backdrop) {
        // The wash over the backdrop comes from --tint alongside the rest of
        // the palette, rather than being a second hard-coded near-black here.
        document.documentElement.style.setProperty('--backdrop-url', "url('/assets/backdrop.png')");
        document.body.classList.add('has-backdrop');
      }
      if (assets.logo) {
        const src = '/assets/' + assets.logo;
        // Small logo in the top bar, which is on screen the whole time.
        const tb = $('#topBrand .logo');
        if (tb) {
          tb.textContent = '';
          const im = el('img'); im.src = src; im.alt = '';
          im.onerror = () => { tb.textContent = '🌱'; };
          tb.append(im);
        }
        // Big logo in the centre of the landing screen; fall back to the seed
        // emoji if the image is missing or broken.
        const hl = $('#heroLogo');
        if (hl) {
          const im = el('img'); im.src = src; im.alt = 'SeedGPT';
          im.onerror = () => { hl.innerHTML = ''; hl.append(el('span', 'logo-emoji', '🌱')); };
          hl.innerHTML = ''; hl.append(im);
        }
        // Small logo beside the sidebar brand.
        const brand = $('.side-brand .logo');
        if (brand) {
          brand.textContent = '';
          const im = el('img'); im.src = src; im.alt = '';
          im.style.cssText = 'width:22px;height:22px;object-fit:contain;image-rendering:pixelated;display:block';
          brand.append(im);
        }
      }
    } catch (e) { /* graphics are optional — the built-in look stands alone */ }
  }
  // ---- query state: share links + saved searches -------------------------
  function currentState() {
    return { v: $('#version').value, r: +$('#range').value,
             t: +$('#threads').value, c: conds,
             // A shared leaderboard link must rebuild as a leaderboard, or the
             // recipient silently gets a plain filter search instead.
             b: budgetSeconds() / 60,
             k: rank.on ? { of: rank.of, by: rank.by, top: rank.top } : null,
             lb: largeBiomes(),
             // The exact query when the builder has not been edited since it
             // was loaded -- the builder cannot express every field, and a link
             // to an AI search must reopen as that search, not a lossy copy.
             q: (loadedQuery && builderSnapshot() === loadedSnap) ? loadedQuery : null };
  }
  function applyState(s) {
    if (!s || !Array.isArray(s.c)) return false;
    if (s.q && Array.isArray(s.q.conditions)) {
      if (s.r) $('#range').value = s.r;
      if (s.t) $('#threads').value = s.t;
      if (s.b !== undefined) $('#budgetMin').value = s.b;
      loadQueryIntoBuilder(s.q);
      return true;
    }
    if (s.v) $('#version').value = s.v;
    if (s.r) $('#range').value = s.r;
    if (s.t) $('#threads').value = s.t;
    if (s.b !== undefined) $('#budgetMin').value = s.b;
    $('#largeBiomes').checked = !!s.lb;
    rank.on = !!s.k;
    if (s.k) { rank.of = s.k.of || ''; rank.by = s.k.by || 'auto'; rank.top = s.k.top || 10; }
    conds = s.c;
    nextId = conds.reduce((m, c) =>
      Math.max(m, parseInt(String(c.id).replace(/\D/g, '')) || 0), 0) + 1;
    return true;
  }
  // The URL hash carries the whole builder state (base64url JSON), so a link
  // rebuilds the query on any local instance -- the server is loopback-only, so
  // "sharing" means sharing the query, not a hosted page.
  function encodeState(s) {
    return btoa(unescape(encodeURIComponent(JSON.stringify(s))))
      .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  function decodeState(str) {
    try {
      return JSON.parse(decodeURIComponent(escape(
        atob(str.replace(/-/g, '+').replace(/_/g, '/')))));
    } catch (e) { return null; }
  }
  // Share: the query rides in the URL hash. The hint promised a Share button
  // on results; there was none, and encodeState had no caller left.
  async function shareLink(btn) {
    const url = location.origin + location.pathname + '#q=' + encodeState(currentState());
    history.replaceState(null, '', url);
    try {
      await navigator.clipboard.writeText(url);
      if (btn) { const was = btn.textContent; btn.textContent = 'link copied';
                 setTimeout(() => { btn.textContent = was; }, 1200); }
      toast('link copied — it reopens this search on any copy of SeedGPT running locally', 'good');
    } catch (_) {
      toast('could not copy; the link is in the address bar now');
    }
  }
  function getSaved() { try { return JSON.parse(localStorage.getItem('sf-saved') || '[]'); } catch (e) { return []; } }
  function setSaved(a) { localStorage.setItem('sf-saved', JSON.stringify(a)); renderSaved(); }
  function renderSaved() {
    const box = $('#savedList'); box.innerHTML = '';
    const a = getSaved();
    if (!a.length) { box.append(el('div', 'hint', 'No saved searches yet.')); return; }
    a.forEach((it, i) => {
      const row = el('div', 'saved');
      const name = el('button', 'tiny', it.name);
      name.title = 'load this search'; name.style.flex = '1';
      // The saved version's vocabulary has to load too, or the builder shows
      // the conditions against another version's structure list.
      name.onclick = async () => {
        const v = it.state && (it.state.q ? it.state.q.version : it.state.v);
        if (v && v !== $('#version').value) { $('#version').value = v; await loadVocab(); }
        if (applyState(it.state)) { render(); toast('loaded “' + it.name + '”'); }
      };
      const del = el('button', 'tiny x', '×'); del.title = 'delete';
      del.onclick = () => { const b = getSaved(); b.splice(i, 1); setSaved(b); };
      row.append(name, del); box.append(row);
    });
  }
  $('#saveBtn').onclick = () => {
    const name = $('#saveName').value.trim();
    if (!name) { toast('name the search first'); return; }
    const a = getSaved();
    const at = a.findIndex(x => x.name === name);
    const entry = { name, state: currentState() };
    if (at >= 0) a[at] = entry; else a.push(entry);
    setSaved(a); $('#saveName').value = '';
    toast('saved “' + name + '”');
  };
  $('#saveName').addEventListener('keydown', e => { if (e.key === 'Enter') $('#saveBtn').click(); });

  // ---- coordinate export -------------------------------------------------
  function seedRows(seeds) {
    const rows = [['seed', 'kind', 'id', 'x', 'z', 'detail']];
    seeds.forEach(s => {
      let any = false;
      const push = (k, id, x, z, d) => { rows.push([s.seed, k, id, x, z, d]); any = true; };
      if (s.spawn)  push('spawn', 'spawn', s.spawn.x, s.spawn.z, '');
      if (s.portal) push('portal', 'end_portal', s.portal.x, s.portal.z, s.portal.eyes + '/12 eyes');
      (s.loot  || []).forEach(l => push('loot', l.id, l.x, l.z, l.count + ' ' + l.item));
      (s.ore   || []).forEach(o => push('ore', o.material, o.x, o.z,
        o.count + (o.exposed ? ' exposed' : '') + ' within ' + o.within
        + (o.vein !== undefined ? ', biggest vein ' + o.vein : '')));
      (s.overlap|| []).forEach(o => {
        push('overlap', o.structure, o.x, o.z, 'overlaps ' + (o.structure2 || '?'));
        if (o.structure2) push('overlap', o.structure2, o.x2, o.z2,
          o.gap + ' blocks away, ' + o.area + ' blocks² overlap');
      });
      (s.stronghold || []).forEach(o => {
        push('stronghold', o.id, o.x, o.z, 'roof y=' + o.roof + ', ' + o.state);
        if (o.tp) push('stronghold_shallowest', o.id, o.tp.x, o.tp.z,
          'y=' + o.tp.y + ', ' + o.depth + ' block(s) of ground over it, ' + o.where);
      });
      (s.cactus || []).forEach(o => push('cactus', o.id, o.x, o.z, o.height + ' tall, base y=' + o.base_y));
      (s.slime || []).forEach(o => push('slime', o.id, o.x, o.z, o.count + ' within ' + o.within));
      (s.area  || []).forEach(o => push('biome_area', o.biome, o.x, o.z, o.pct + '% within ' + o.within));
      (s.height|| []).forEach(o => push('height', 'terrain', o.x, o.z,
        (o.exact ? '' : '~') + o.peak + ' peak' + (o.exact ? ' (exact)' : ' (approx)')));
      (s.biome || []).forEach(o => push('biome', o.biome, o.x, o.z, o.dist + ' from ' + o.ref));
      (s.cluster|| []).forEach(o => push('cluster', o.structure, o.x, o.z,
        o.tight ? o.count + ' within ' + o.spread + ' of each other' : o.count + ' within ' + o.within));
      (s.places|| []).forEach(p => push('structure', p.id, p.x, p.z, p.dist + ' from ' + (p.ref || 'origin')));
      if (!any) rows.push([s.seed, 'seed', '', '', '', '']);
    });
    return rows;
  }
  const toCSV = rows => rows.map(r => r.map(v => {
    const s = v == null ? '' : String(v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }).join(',')).join('\n');
  function download(name, text, type) {
    const a = el('a'); a.href = URL.createObjectURL(new Blob([text], { type }));
    a.download = name; document.body.append(a); a.click(); a.remove();
    URL.revokeObjectURL(a.href);
  }
  function exportBar(seeds) {
    const bar = el('div', 'exportbar');
    bar.append(el('span', 'exl', 'export'));
    const csv = el('button', 'tiny', '⬇ CSV');
    csv.onclick = () => download('seeds.csv', toCSV(seedRows(seeds)), 'text/csv');
    const json = el('button', 'tiny', '⬇ JSON');
    json.onclick = () => download('seeds.json', JSON.stringify(seeds, null, 2), 'application/json');
    const copy = el('button', 'tiny', 'copy CSV');
    copy.onclick = async () => {
      try { await navigator.clipboard.writeText(toCSV(seedRows(seeds)));
        copy.textContent = 'copied'; setTimeout(() => copy.textContent = 'copy CSV', 900); } catch (e) {}
    };
    bar.append(csv, json, copy);
    return bar;
  }

  // ---- one run per tool button ------------------------------------------
  // The single-shot tools (some boot a Minecraft or Bedrock server) could be
  // started twice by a double-click, and nothing showed one was running. Each
  // button is disabled for exactly as long as its own request runs. The hunt
  // buttons manage their own state and are not wrapped.
  ['#boBtn', '#csBtn', '#etBtn', '#flBtn', '#foBtn', '#soBtn', '#svBtn', '#vsBtn',
   '#tcBtn', '#keyTest'].forEach(id => {
    const b = $(id); if (!b || !b.onclick) return;
    const handler = b.onclick;
    b.onclick = async e => {
      if (b.dataset.running) return;
      b.dataset.running = '1'; b.disabled = true; b.setAttribute('aria-busy', 'true');
      try { await handler.call(b, e); }
      finally { delete b.dataset.running; b.disabled = false; b.removeAttribute('aria-busy'); }
    };
  });

  // ---- start -------------------------------------------------------------
  loadAssets();
  renderSaved();
  // A shared link (#q=...) rebuilds its query. Its version is set BEFORE the
  // vocabulary loads, so the builder checks the conditions against the right
  // version's structures instead of discarding ones the default lacks.
  const shareHash = location.hash.match(/^#q=(.+)$/);
  const shared = shareHash ? decodeState(shareHash[1]) : null;
  const sharedV = shared && (shared.q ? shared.q.version : shared.v);
  if (sharedV && Array.from($('#version').options).some(o => o.value === sharedV))
    $('#version').value = sharedV;
  loadVocab().then(() => {
    // Otherwise start from a demo.
    let restored = false;
    if (shared && applyState(shared)) restored = true;
    if (!restored) {
      conds = [
        { id:'mansion', kind:'structure', name:'mansion', within:300, of:'origin' },
        { id:'village', kind:'structure', name:'village', within:400, of:'mansion' },
      ];
      nextId = 3;
    }
    render();
  });
})();
