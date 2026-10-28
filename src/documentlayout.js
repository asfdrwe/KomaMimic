// SPDX-License-Identifier: GPL-3.0-only
// Trusted reader code. Runs only in Qt's isolated ApplicationWorld.
// Publication iframes have allow-same-origin, without allow-scripts.
window.komaDocument = (() => {
    const W = 1200, H = 1600;
    let frames = [], pages = [], current = 0, ready = false, error = '', changed = false;
    let messages = {};
    const message = id => messages[id] || id;
    let serial = 0, note = -1, notes = [], history = [], controls;
    const remember = () => { history.push({current,note}); if(history.length>100)history.shift(); };
    function back() { const old=history.pop(); if(!old)return; if(old.note>=0)showNote(old.note); else select(old.current); }
    let pageLayout = 0, rightToLeft = false, displayed = [];
    function textNodes(doc) {
        const nodes = [];
        let offset = 0;
        function visit(node) {
            if(node.nodeType===Node.ELEMENT_NODE) {
                if(node.matches('rt,rp,script,style,noscript,head'))return;
                if(!doc.documentElement.dataset.epub && node.hasAttribute('data-koma-offset'))offset=Math.max(0,Number(node.getAttribute('data-koma-offset'))||0);
                if(node.tagName==='BR') {++offset;return;}
                for(const child of node.childNodes)visit(child);
            } else if(node.nodeType===Node.TEXT_NODE) {
                if(!node.textContent.trim() && node.textContent.includes('\n') && !node.parentElement.closest('pre'))return;
                if(nodes.length>=200000)throw new Error(message("nodeLimit"));
                nodes.push({node,offset});offset+=node.length;
            }
        }
        visit(doc.body);
        return nodes;
    }
    function pageAt(frame, range) {
        const rect = range.getBoundingClientRect();
        const page=Math.max(0, Math.floor((frame.vertical ? rect.top + frame.el.contentWindow.scrollY : rect.left + frame.el.contentWindow.scrollX) / (frame.vertical ? H : W) + .0001));
        if(!Number.isFinite(page) || page>10000)throw new Error(message("rangeLimit"));
        return page;
    }
    function offsets(frame) {
        const doc = frame.el.contentDocument, result = [0];
        frame.nodes = textNodes(doc);
        let canonical='';
        for(const n of frame.nodes) {
            if(canonical.length<n.offset)canonical+='\n'.repeat(n.offset-canonical.length);
            canonical=canonical.slice(0,n.offset)+n.node.textContent;
        }
        if(frame.section.epub)frame.section.plain=canonical;
        const range = doc.createRange();
        for (const {node, offset} of frame.fixed ? [] : frame.nodes) {
            if (!node.length) continue;
            const at = i => {
                range.setStart(node, i); range.setEnd(node, Math.min(node.length, i + 1));
                return pageAt(frame, range);
            };
            let lo = 0, last = at(node.length - 1);
            for (let p = at(0); p <= last; ++p) {
                let l = lo, r = node.length - 1;
                while (l < r) { const m = (l + r) >> 1; if (at(m) < p) l = m + 1; else r = m; }
                if (result[p] === undefined) result[p] = offset + l;
                lo = l;
            }
        }
        const root = doc.documentElement;
        const count = frame.fixed ? 1 : Math.max(1, Math.ceil((frame.vertical ? root.scrollHeight : root.scrollWidth) / (frame.vertical ? H : W)));
        if (!Number.isFinite(count) || count > 10000 || pages.length + notes.length + count > 20000) throw new Error(message("pageLimit"));
        for (let i = 0; i < count; ++i) {
            const offset = result[i] ?? result.slice(0, i).reverse().find(x => x !== undefined) ?? 0;
            (frame.section.auxiliary ? notes : pages).push({section: frame.section.path, offset, quote: frame.section.plain.slice(offset,offset+40), frame: frames.indexOf(frame), local: i, wide: !!frame.section.wide, side: i===0 ? frame.section.side : 0, solo: !!frame.section.solo});
        }
    }
    function fit() {
        for (const panel of displayed) {
            const g = window.komaPanels.geometry(innerWidth, innerHeight, pageLayout, panel.slot, panel.wide);
            if(panel.solo)g.left=(innerWidth-g.width)/2;
            const content = window.komaPanels.contentGeometry(g, panel.el.offsetWidth, panel.el.offsetHeight);
            panel.el.style.transform = `scale(${content.scale})`;
            panel.el.style.left = `${content.left}px`;
            panel.el.style.top = `${content.top}px`;
        }
    }
    function select(index) {
        if (!ready || !pages.length) return;
        note=-1;
        current = Math.max(0, Math.min(pages.length - 1, index));
        display(current,pages);
    }
    function showNote(index) {
        if(!ready || !notes[index])return;
        note=index; display(note,notes);
    }
    function display(index,list) {
        for (const panel of displayed) panel.el.style.visibility = 'hidden';
        displayed = [];
        const used = new Set();
        for (const panel of window.komaPanels.plan(index, list.length, pageLayout, rightToLeft, list.map(p=>p.wide),list.map(p=>p.side),list.map(p=>p.solo))) {
            const p = list[panel.page], f = frames[p.frame];
            // Two consecutive pages may belong to the same chapter. Each pane
            // needs an independent viewport and scroll position in that case.
            const el = used.has(f) ? f.mirror : f.el;
            used.add(f);
            el.style.visibility = 'visible';
            el.contentWindow.scrollTo(f.vertical ? 0 : p.local * W, f.vertical ? p.local * H : 0);
            displayed.push({...panel, el});
        }
        fit();
        if(controls)controls.style.display=history.length || note>=0 ? 'flex' : 'none';
        changed = true;
    }
    function locate(location,list=pages) {
        const section=frames.find(f=>f.section.path===location.section)?.section;
        if(section && location.quote && section.plain.slice(location.offset,location.offset+location.quote.length)!==location.quote) {
            const found=section.plain.indexOf(location.quote,Math.max(0,location.offset-4096));
            if(found>=0)location={...location,offset:found};
        }
        let index = list.findIndex(p => p.section === location.section);
        if (index < 0) return 0;
        for (let i = index; i < list.length && list[i].section === location.section; ++i) {
            if (list[i].offset > (location.offset || 0)) break;
            index = i;
        }
        return index;
    }
    async function start(spec) {
        const run = ++serial;
        messages = spec.messages || {};
        ready = false; error = ''; pages = []; notes=[]; note=-1; history=[]; current = 0;
        pageLayout = spec.options.pageLayout === 1 ? 1 : 0;
        rightToLeft = !!spec.rightToLeft;
        document.body.replaceChildren(); frames = []; displayed = [];
        document.body.style.cssText = `margin:0;overflow:hidden;background:${spec.options.background}`;
        try {
            controls=document.createElement('div');
            controls.style.cssText='position:fixed;right:8px;top:8px;z-index:100;display:none;gap:6px';
            for(const [label,fn] of [[message("back"),back],[message("notePrevious"),()=>note>=0&&showNote(Math.max(0,note-1))],[message("noteNext"),()=>note>=0&&showNote(Math.min(notes.length-1,note+1))]]) {
                const button=document.createElement('button');button.textContent=label;button.onclick=fn;controls.append(button);
            }
            document.body.append(controls);
            for (const section of spec.sections) {
                if (run !== serial) return;
                const frameWidth = section.wide ? 2*W+24 : W;
                const viewportWidth = section.fixed ? section.width || frameWidth : frameWidth;
                const viewportHeight = section.fixed ? section.height || H : H;
                if (viewportWidth > 16384 || viewportHeight > 16384 || viewportWidth * viewportHeight > 48000000)
                    throw new Error(message("rangeLimit"));
                const makeFrame = () => {
                    const el = document.createElement('iframe');
                    el.sandbox = 'allow-same-origin';
                    el.style.cssText = `position:absolute;top:0;width:${viewportWidth}px;height:${viewportHeight}px;border:0;transform-origin:top left;visibility:hidden;background:${spec.options.background}`;
                    return el;
                };
                section.epub=spec.epub;
                const f = {el: makeFrame(), mirror: pageLayout === 1 && !section.fixed ? makeFrame() : null,
                           section, fixed: section.fixed, vertical: false, nodes: []};
                frames.push(f);
                const prepare = async el => {
                    const loaded = new Promise((resolve, reject) => {
                        el.onload = resolve; el.onerror = () => reject(new Error(message("loadFailed")));
                        setTimeout(() => reject(new Error(message("loadTimeout"))), 15000);
                    });
                    el.src = spec.base + section.path.split('/').map(encodeURIComponent).join('/');
                    document.body.append(el);
                    await loaded;
                    if (run !== serial) return;
                    const doc = el.contentDocument;
                    if (!doc || !doc.body) throw new Error(message("parseFailed"));
                    if(spec.epub)doc.documentElement.dataset.epub='true';
                    if (!spec.options.publisherStyles) {
                        doc.querySelectorAll('style,link[rel="stylesheet"]').forEach(e => e.remove());
                        doc.querySelectorAll('[style]').forEach(e=>e.removeAttribute('style'));
                    }
                    const style = doc.createElement('style'), o = spec.options;
                    let css = `html{overflow:hidden!important} body{overflow:visible!important} rt{font-size:50%} img,svg{max-width:100%;max-height:100%} pre{white-space:pre-wrap;overflow-wrap:anywhere}`;
                    // Trusted reader typography survives publisher-style removal.
                    // This is supplied only for converted Markdown, never EPUB.
                    if (o.markdownStyle) css += o.markdownStyle;
                    // Reader image-page sizing must survive publisher CSS being
                    // disabled. These rules are part of the trusted layout.
                    if (section.imagePage) css += 'html,body{margin:0;width:100%;height:100%}img{width:100%;height:100%;object-fit:contain;display:block}';
                    if (!o.ruby) css += 'rt,rp{display:none!important}';
                    if (!section.fixed) {
                        const mode = o.writingMode === 2 ? 'vertical-rl' : o.writingMode === 1 ? 'horizontal-tb' : '';
                        if (mode) css += `html,body{writing-mode:${mode}!important}`;
                        css += `body{font-size:${o.size}px!important;line-height:${o.size + o.spacing}px!important}`;
                        // Inline priority also overrides publisher class rules and
                        // existing inline families, while leaving other CSS intact.
                        if (o.family) {
                            for (const node of [doc.body, ...doc.body.querySelectorAll('*')])
                                node.style?.setProperty('font-family', JSON.stringify(o.family), 'important');
                        }
                        if (o.overrideColors || !spec.epub) css += `html,body{color:${o.foreground}!important;background:${o.background}!important}`;
                    }
                    style.textContent = css; doc.head.append(style);
                    const vertical = !section.fixed && getComputedStyle(doc.body).writingMode.startsWith('vertical');
                    if (el === f.el) f.vertical = vertical;
                    if (!section.fixed) {
                        const m = o.margin;
                        const sheet = doc.createElement('style');
                        sheet.textContent = `html{margin:0!important;padding:0!important;width:${W}px!important;height:${H}px!important}body{box-sizing:content-box!important;margin:${m}px!important;padding:0!important;width:${W-2*m}px!important;height:${H-2*m}px!important;min-width:0!important;max-width:none!important;column-width:${vertical ? H-2*m : W-2*m}px!important;column-gap:${2*m}px!important;column-fill:auto!important} p{max-width:100%}`;
                        doc.head.append(sheet);
                    } else {
                        // Keep the document's layout viewport at its authored
                        // size. Scaling body inside a larger iframe leaves a
                        // vertical-rl root anchored at that iframe's right edge
                        // and clips its text/image. Scale the outer iframe in fit().
                        doc.body.style.cssText += `;margin:0!important;width:${viewportWidth}px!important;height:${viewportHeight}px!important`;
                    }
                    const images=[...doc.images];
                    images.forEach(img=>{img.loading='eager';});
                    await Promise.race([Promise.all(images.map(img=>img.decode().catch(()=>{}))),new Promise(resolve=>setTimeout(resolve,3000))]);
                    await Promise.race([doc.fonts.ready, new Promise(resolve => setTimeout(resolve, 3000))]);
                    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
                    doc.addEventListener('click', event => {
                        const a = event.target.closest('a[href]');
                        if (!a) return;
                        event.preventDefault();
                        const url = new URL(a.href);
                        if (!url.href.startsWith(spec.base)) return;
                        const path = decodeURIComponent(url.pathname.slice(1));
                        const target = frames.find(x => x.section.path === path);
                        if (!target) return;
                        navigate({section:path,fragment:decodeURIComponent(url.hash.slice(1)),offset:0});
                    }, true);
                };
                await Promise.all([f.el, f.mirror].filter(Boolean).map(prepare));
                if (run !== serial) return;
                offsets(f);
            }
            if (run !== serial) return;
            fit(); ready = true; select(locate(spec.location || {}));
        } catch (e) { if (run === serial) error = String(e.message || e); }
    }
    addEventListener('resize', fit);
    function navigate(location) {
        const target=frames.find(f=>f.section.path===location.section);
        if(!ready || !target)return;
        remember();
        const auxiliary=target.section.auxiliary;
        const list=auxiliary ? notes : pages;
        const show=auxiliary ? showNote : select;
        if (location.fragment) {
            const f = frames.find(x => x.section.path === location.section);
            const element = f?.el.contentDocument.getElementById(location.fragment);
            if (element) {
                const range=f.el.contentDocument.createRange(); range.selectNodeContents(element);
                const local=pageAt(f,range);
                const index=list.findIndex(p=>p.section===location.section && p.local===local);
                if(index>=0) {show(index); return;}
            }
        }
        show(locate(location,list));

    }
    return {start,select,navigate,back, stepNote(direction) { if(note>=0)showNote(Math.max(0,Math.min(notes.length-1,note+direction))); }, state(includePages) {
        const result = {ready, error, current, changed, auxiliary:note>=0, noteSection:notes[note]?.section || '', vertical:!!frames[pages[current]?.frame]?.vertical, visible:displayed.map(p => p.page), pages:includePages ? pages : []}; changed = false; return result;
    }, find(text) {
        if (!ready || !text) return false;
        const start = current + 1;
        for (let n = 0; n < pages.length; ++n) {
            const index = (start + n) % pages.length, p = pages[index], f = frames[p.frame];
            const all = f.section.plain;
            const found = all.toLocaleLowerCase().indexOf(text.toLocaleLowerCase(), p.offset);
            const end = pages[index+1]?.section === p.section ? pages[index+1].offset : Infinity;
            if (found >= p.offset && found < end) { select(index); return true; }
        }
        return false;
    }};
})();
