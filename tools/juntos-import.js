// Dionisio → Juntos+ cart import.
// Paste into a "custom JavaScript for websites" extension, scoped to https://br.juntosplus.com/*.
// Adds a "Importar do Dionisio" button that fills the cart with this cycle's JUNTOS suggestions:
//  1. reads the suggestions from Dionisio;
//  2. maps each name to a Juntos+ product through your order history (the export has names, not SKUs,
//     and several products share a name, so the one you bought most recently wins); names never ordered
//     fall back to search, used only when exactly one product has that name;
//  3. opens each product page, types the number of boxes and presses "Adicionar". That sets the cart
//     quantity (it does not add to it), so running it twice is safe. It never finishes the order.
(() => {
  const DIONISIO_URL = 'https://dionisio-xsujjjd2wq-uc.a.run.app';
  const DIONISIO_TOKEN = 'PASTE_API_TOKEN_HERE';

  if (window.__dionisioImport) return;
  window.__dionisioImport = true;

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
  async function until(fn, ms = 10000) {
    for (const end = Date.now() + ms; Date.now() < end; await sleep(200)) {
      const v = fn();
      if (v) return v;
    }
    return null;
  }
  function go(path) { // client-side navigation: the page (and this script) stays loaded
    history.pushState({}, '', path);
    dispatchEvent(new PopStateEvent('popstate'));
  }
  async function api(path, body) {
    const r = await fetch(DIONISIO_URL + path, {
      method: body ? 'POST' : 'GET',
      headers: { Authorization: 'Bearer ' + DIONISIO_TOKEN, ...(body && { 'Content-Type': 'application/json' }) },
      body: body && JSON.stringify(body),
    });
    if (!r.ok) throw new Error(`Dionisio ${path}: HTTP ${r.status}${r.status === 401 ? ' (check DIONISIO_TOKEN)' : ''}`);
    return r.json();
  }

  // The site's Redux store, found through React's internal fiber tree.
  function reduxStore() {
    const root = document.querySelector('#root');
    const key = root && Object.keys(root).find((k) => k.startsWith('__reactContainer$'));
    const stack = key ? [root[key]] : [];
    for (let n = 0; stack.length && n < 50000; n++) {
      const f = stack.pop();
      if (f.memoizedProps?.store?.getState) return f.memoizedProps.store;
      if (f.sibling) stack.push(f.sibling);
      if (f.child) stack.push(f.child);
    }
    return null;
  }

  // normalized name -> {sku, per, size} from the most recent order containing it
  async function productsFromHistory() {
    go('/business/orders?type=history');
    const store = await until(reduxStore);
    const data = store && await until(() => {
      const qs = store.getState().juntosFrontDoorApi?.queries || {};
      return Object.entries(qs).find(([k, q]) => k.startsWith('getHistoryOrders') && q.data)?.[1].data;
    });
    if (!data) throw new Error('could not read the Juntos+ order history');
    const orders = [...data.pedidoHistoricoCabecera]
      .sort((a, b) => (b.fechaPedido + b.horaPedido).localeCompare(a.fechaPedido + a.horaPedido));
    const map = {};
    for (const o of orders)
      for (const d of o.pedidoHistoricoDetalle)
        for (const m of d.materiales)
          map[norm(m.descripcionMaterial)] ??= { sku: m.idMaterial, per: +m.presentacion, size: m.medidaPresentacion };
    return map;
  }

  async function searchUnique(name) {
    go('/search?product=' + encodeURIComponent(name));
    await sleep(3000); // let the results render
    const skus = new Set([...document.querySelectorAll('a[href^="/product/"]')] // card links: text or <img alt>
      .filter((x) => [x.textContent, x.querySelector('img')?.alt || ''].some((t) => norm(t) === norm(name)))
      .map((x) => x.getAttribute('href').match(/\/product\/(\d+)/)?.[1]));
    if (skus.size > 1) throw new Error(`never ordered, and ${skus.size} products have this name: order it once by hand`);
    if (!skus.size) throw new Error('not found on Juntos+');
    return { sku: [...skus][0] };
  }

  async function addItem(s, history) {
    const p = history[norm(s.item)] || await searchUnique(s.item);
    // Product-to-product navigation reuses the page and keeps the previous quantity in its state (an equal
    // new quantity then never enables "Adicionar"); passing through another route gives a fresh page.
    go('/help');
    await sleep(300);
    go(`/product/${p.sku}?product_type=product`);
    const add = await until(() => norm(document.title).endsWith(norm(s.item)) && // "Detalhes do produto <name>"
      [...document.querySelectorAll('button')].find((b) => b.textContent.trim() === 'Adicionar'));
    if (!add) throw new Error(`product page ${p.sku} did not load`);

    let boxes = s.suggested;
    if (s.unknown_pack) { // Dionisio's number is units: convert to boxes and teach Dionisio the box size
      let per = p.per;
      for (let el = add; !per && el; el = el.parentElement)
        per = +(el.textContent.match(/(\d+)\s*unidades/i)?.[1] || 0);
      if (!per) throw new Error('box size unknown');
      boxes = Math.ceil(s.suggested / per);
      await api(`/api/items/${s.item_id}`, { pack_size: per }).catch(() => {});
    }

    const input = document.querySelector('button[aria-label="Add product"]')?.parentElement.querySelector('input');
    if (!input) throw new Error('quantity box not found');
    // React only sees a typed value through the native setter + input event; it commits on blur.
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, String(boxes));
    input.focus();
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.blur();
    const ready = await until(() => [...document.querySelectorAll('button')]
      .find((b) => b.textContent.trim() === 'Adicionar' && !b.disabled), 2000);
    if (!ready) throw new Error('"Adicionar" stayed disabled (out of stock or over the limit?)');
    ready.click();
    await sleep(800);
    return `${boxes} cx${p.size ? ` × ${p.per} · ${p.size}` : ''}`;
  }

  // ---- UI ----
  const btn = document.createElement('button');
  btn.textContent = '🍷 Importar do Dionisio';
  btn.style.cssText = 'position:fixed;right:20px;bottom:20px;z-index:2147483647;padding:12px 18px;border:0;' +
    'border-radius:24px;background:#d45800;color:#fff;font:600 14px system-ui;box-shadow:0 4px 14px #0004;cursor:pointer';
  const panel = document.createElement('pre');
  panel.style.cssText = 'position:fixed;right:20px;bottom:72px;z-index:2147483647;max-height:60vh;width:460px;' +
    'overflow:auto;margin:0;padding:12px;border-radius:10px;background:#14161c;color:#e3e6eb;' +
    'font:12px/1.5 ui-monospace,monospace;white-space:pre-wrap;display:none;box-shadow:0 4px 14px #0006';
  panel.title = 'click to close';
  panel.onclick = () => { panel.style.display = 'none'; };
  const log = (line) => { panel.style.display = 'block'; panel.textContent += line + '\n'; panel.scrollTop = 1e9; };
  document.body.append(btn, panel);

  btn.onclick = async () => {
    if (DIONISIO_TOKEN.startsWith('PASTE')) return log('✗ Set DIONISIO_TOKEN at the top of the script first.');
    btn.disabled = true;
    panel.textContent = '';
    try {
      const home = await api('/api/home');
      const todo = home.suggestions.filter((s) => s.supplier === 'JUNTOS' && s.suggested > 0);
      log(`Cycle ${home.cycle.start}: ${todo.length} JUNTOS items\n`);
      const history = await productsFromHistory();
      let failed = 0;
      for (const [i, s] of todo.entries()) {
        btn.textContent = `🍷 ${i + 1}/${todo.length}…`;
        try {
          log(`✓ ${await addItem(s, history)}  ${s.item}`);
        } catch (e) {
          failed++;
          log(`✗ ${s.item}: ${e.message}`);
        }
      }
      log(`\nDone: ${todo.length - failed} added, ${failed} failed. Review the cart before finishing the order.`);
      go('/cart');
    } catch (e) {
      log('✗ ' + e.message);
    } finally {
      btn.disabled = false;
      btn.textContent = '🍷 Importar do Dionisio';
    }
  };
})();
