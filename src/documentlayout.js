// SPDX-License-Identifier: GPL-3.0-only
// Trusted reader code, isolated from publication scripts.
window.komaDocument = (() => {
    const W = 1200, H = 1600;
    let frames = [], pages = [], notes = [], displayed = [], current = 0, note = -1;
    let ready = false, changed = false, error = '', serial = 0, navigation = 0, revision = 0;
    let spec, controls, messages = {}, history = [], inferredDirection = false, boundary = 0;
    const message = id => messages[id] || id;
    const canonicalPath = s => s.locationPath || s.path;
    const active = (run,nav) => run===serial && nav===navigation;
    const remember = () => { history.push({location: locationOf(note >= 0 ? notes[note] : pages[current]), auxiliary: note >= 0}); if (history.length > 100) history.shift(); };
    const locationOf = p => p ? {section:p.section, offset:p.offset, quote:p.quote || ''} : {};
    function textNodes(doc) {
        const nodes = []; let offset = 0;
        function visit(node) {
            if (node.nodeType === Node.ELEMENT_NODE) {
                if (node.matches('rt,rp,script,style,noscript,head')) return;
                if (!doc.documentElement.dataset.epub && node.hasAttribute('data-koma-offset')) offset = Math.max(0,Number(node.getAttribute('data-koma-offset')) || 0);
                if (node.tagName === 'BR') { ++offset; return; }
                for (const child of node.childNodes) visit(child);
            } else if (node.nodeType === Node.TEXT_NODE) {
                if (!node.textContent.trim() && node.textContent.includes('\n') && !node.parentElement.closest('pre')) return;
                if (nodes.length >= 200000) throw new Error(message('nodeLimit'));
                nodes.push({node,offset}); offset += node.length;
            }
        }
        visit(doc.body); return nodes;
    }
    function pageAt(frame, range) {
        const rect = range.getBoundingClientRect();
        const page = Math.max(0,Math.floor((frame.vertical ? rect.top+frame.el.contentWindow.scrollY : rect.left+frame.el.contentWindow.scrollX)/(frame.vertical ? H : W)+.0001));
        if (!Number.isFinite(page) || page > 10000) throw new Error(message('rangeLimit'));
        return page;
    }
    function offsets(frame) {
        const doc=frame.el.contentDocument, result=[frame.section.baseOffset || 0];
        frame.nodes=textNodes(doc);
        if(spec.epub){let text='';for(const n of frame.nodes){if(text.length<n.offset)text+='\n'.repeat(n.offset-text.length);text=text.slice(0,n.offset)+n.node.textContent;}frame.section.plain=text;}
        const range=doc.createRange();
        for (const {node,offset} of frame.fixed ? [] : frame.nodes) {
            if (!node.length) continue;
            const at=i=>{range.setStart(node,i);range.setEnd(node,Math.min(node.length,i+1));return pageAt(frame,range);};
            let lo=0; const last=at(node.length-1);
            for(let p=at(0);p<=last;++p) {
                let l=lo,r=node.length-1;
                while(l<r){const m=(l+r)>>1;if(at(m)<p)l=m+1;else r=m;}
                if(result[p]===undefined)result[p]=offset+l;lo=l;
            }
        }
        const root=doc.documentElement;
        const count=frame.fixed ? 1 : Math.max(1,Math.ceil((frame.vertical ? root.scrollHeight : root.scrollWidth)/(frame.vertical ? H : W)));
        if(!Number.isFinite(count) || count>10000)throw new Error(message('pageLimit'));
        const list=frame.section.auxiliary ? notes : pages;
        const start=list.findIndex(p=>p.frame===frame.index), oldCount=list.filter(p=>p.frame===frame.index).length;
        if(pages.length+notes.length-oldCount+count>20000)throw new Error(message('pageLimit'));
        const replacement=[];
        for(let i=0;i<count;++i) {
            const offset=result[i] ?? result.slice(0,i).reverse().find(x=>x!==undefined) ?? (frame.section.baseOffset || 0);
            replacement.push({section:canonicalPath(frame.section),offset,quote:(frame.section.plain || '').slice(offset-(frame.section.baseOffset || 0),offset-(frame.section.baseOffset || 0)+40),
                frame:frame.index,local:i,wide:frame.wide,side:i===0?frame.section.side:0,solo:frame.solo || (i===0 && frame.coverSolo),rightToLeft:frame.rightToLeft,pending:false});
        }
        const oldLocation=locationOf(pages[current]);
        list.splice(start,oldCount,...replacement);++revision;
        current=locate(oldLocation,pages);
    }
    function fit() {
        for(const panel of displayed) {
            const g=window.komaPanels.geometry(innerWidth,innerHeight,panel.solo ? 0 : spec.options.pageLayout,panel.solo ? 0 : panel.slot,panel.wide);
            const content=window.komaPanels.contentGeometry(g,panel.el.offsetWidth,panel.el.offsetHeight);
            panel.el.style.transform=`scale(${content.scale})`;panel.el.style.left=`${content.left}px`;panel.el.style.top=`${content.top}px`;
        }
    }
    function locate(location,list=pages) {
        let offset=location.offset || 0;
        if(location.quote)for(const f of frames)if(canonicalPath(f.section)===location.section && f.loaded) {
            const base=f.section.baseOffset || 0,plain=f.section.plain || '',at=offset-base;
            if(at>=0 && at<=plain.length && plain.slice(at,at+location.quote.length)!==location.quote){const found=plain.indexOf(location.quote,Math.max(0,at-4096));if(found>=0)offset=base+found;}
        }
        let index=list.findIndex(p=>p.section===location.section);if(index<0)return 0;
        for(let i=index;i<list.length;++i) {
            if(list[i].section!==location.section)continue;
            if(list[i].offset>offset)break;index=i;
        }
        return index;
    }
    function plan(index,list) {
        const p=list[index];if(!p)return [];
        const solo=list.map((item,i)=>item.solo || item.pending || (i>index && item.rightToLeft!==p.rightToLeft));
        return window.komaPanels.plan(index,list.length,spec.options.pageLayout,p.rightToLeft,list.map(p=>p.wide),list.map(p=>p.side),solo);
    }
    function makeElement(frame) {
        const el=document.createElement('iframe');el.sandbox='allow-same-origin';
        el.style.cssText=`position:absolute;top:0;width:${W}px;height:${H}px;border:0;transform-origin:top left;visibility:hidden;background:${spec.options.background}`;
        return el;
    }
    async function prepare(frame,el,run) {
        const loaded=new Promise((resolve,reject)=>{
            const timer=setTimeout(()=>reject(new Error(message('loadTimeout'))),15000);
            el.onload=()=>{clearTimeout(timer);resolve();};el.onerror=()=>{clearTimeout(timer);reject(new Error(message('loadFailed')));};
        });
        el.src=spec.base+frame.section.path.split('/').map(encodeURIComponent).join('/');document.body.append(el);await loaded;
        if(run!==serial) return;
        const doc=el.contentDocument;if(!doc || !doc.body)throw new Error(message('parseFailed'));
        const metadata=doc.head.querySelector('meta[name="koma-section"]');
        if(!metadata)throw new Error(message('loadFailed'));
        Object.assign(frame.section,JSON.parse(metadata.content));
        const s=frame.section,o=spec.options;
        if(spec.epub && !s.authoredFixed && o.writingMode)s.side=0;
        if(s.error)throw new Error(s.error);
        if(spec.epub)doc.documentElement.dataset.epub='true';
        if(!o.publisherStyles){doc.querySelectorAll('style,link[rel="stylesheet"]').forEach(e=>e.remove());doc.querySelectorAll('[style]').forEach(e=>e.removeAttribute('style'));}
        const images=[...doc.images];images.forEach(img=>{img.loading='eager';});
        await Promise.race([Promise.all(images.map(img=>img.decode().catch(()=>{}))),new Promise(resolve=>setTimeout(resolve,3000))]);
        if(run!==serial)return;
        for(const img of images)if(!img.naturalWidth){const fallback=doc.createElement('span');fallback.textContent=img.dataset.komaFallback || img.alt || message('imageFailed');img.replaceWith(fallback);}
        const onlyImage=!!s.imagePage;
        const image=onlyImage && images.length===1 ? images[0] : null;
        const imageWide=image && window.komaPanels.isWide(image.naturalWidth,image.naturalHeight);
        frame.wide=!!o.wideImages && (image ? !!imageWide : !!s.wide);
        frame.solo=!!s.solo;frame.coverSolo=!!s.cover && !!o.coverAlone;
        frame.fixed=!!s.fixed || onlyImage;
        const width=frame.wide ? 2424 : s.width || W, height=s.height || H;
        if(width>16384 || height>16384 || width*height>48000000)throw new Error(message('rangeLimit'));
        el.style.width=`${frame.fixed ? width : W}px`;el.style.height=`${frame.fixed ? height : H}px`;
        const style=doc.createElement('style');
        let css='html{overflow:hidden!important}body{overflow:visible!important}rt{font-size:50%}img,svg{max-width:100%;max-height:100%}pre{white-space:pre-wrap;overflow-wrap:anywhere}';
        if(o.markdownStyle)css+=o.markdownStyle;
        if(onlyImage)css+='html,body{margin:0!important;width:100%;height:100%}body{display:flex;flex-direction:column}.koma-picture-image{flex:1;min-height:0;min-width:0}img{width:100%;height:100%;object-fit:contain;display:block}.koma-picture-caption{flex:none;text-align:center;font-size:22px;line-height:30px}';
        if(!o.ruby)css+='rt,rp{display:none!important}';
        if(!frame.fixed) {
            const mode=o.writingMode===2?'vertical-rl':o.writingMode===1?'horizontal-tb':'';
            if(mode)css+=`html,body{writing-mode:${mode}!important}`;
            css+=`body{font-size:${o.size}px!important;line-height:${o.size+o.spacing}px!important}`;
            if(o.family)for(const node of [doc.body,...doc.body.querySelectorAll('*')])node.style?.setProperty('font-family',JSON.stringify(o.family),'important');
            if(o.overrideColors || !spec.epub)css+=`html,body{color:${o.foreground}!important;background:${o.background}!important}`;
        }
        style.textContent=css;doc.head.append(style);
        const flowVertical=getComputedStyle(doc.body).writingMode.startsWith('vertical');
        frame.vertical=!frame.fixed && flowVertical;
        if(!frame.fixed)inferredDirection=frame.vertical;
        frame.rightToLeft=window.komaPanels.direction(!!spec.epub,!!spec.directionSpecified,!!spec.rightToLeft,o.writingMode,!!s.authoredFixed,frame.fixed && onlyImage?inferredDirection:flowVertical);
        if(!frame.fixed) {
            const m=o.margin,sheet=doc.createElement('style');
            sheet.textContent=`html{margin:0!important;padding:0!important;width:${W}px!important;height:${H}px!important}body{box-sizing:content-box!important;margin:${m}px!important;padding:0!important;width:${W-2*m}px!important;height:${H-2*m}px!important;min-width:0!important;max-width:none!important;column-width:${frame.vertical?H-2*m:W-2*m}px!important;column-gap:${2*m}px!important;column-fill:auto!important}p{max-width:100%}`;
            doc.head.append(sheet);
        } else doc.body.style.cssText+=`;margin:0!important;width:${width}px!important;height:${height}px!important`;
        await Promise.race([doc.fonts.ready,new Promise(resolve=>setTimeout(resolve,3000))]);
        await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
        if(run!==serial)return;
        doc.addEventListener('click',event=>{
            const a=event.target.closest('a[href]');if(!a)return;event.preventDefault();
            const url=new URL(a.href);if(!url.href.startsWith(spec.base))return;
            navigate({section:decodeURIComponent(url.pathname.slice(1)),fragment:decodeURIComponent(url.hash.slice(1)),offset:0});
        },true);
    }
    async function ensureFrame(frame,run) {
        while(frame.loading) {
            const loadingRun=frame.loadingRun;
            try{await frame.loading;}catch(e){if(loadingRun===run)throw e;}
        }
        if(run!==serial || (frame.el && frame.loaded))return;
        frame.loadingRun=run;
        frame.loading=(async()=>{
            frame.el?.remove();frame.mirror?.remove();frame.mirror=null;frame.mirrorLoaded=false;frame.el=makeElement(frame);
            await prepare(frame,frame.el,run);if(run!==serial)return;
            offsets(frame);frame.loaded=true;
        })();
        try{await frame.loading;}finally{frame.loading=null;}
    }
    async function ensureMirror(frame,run) {
        while(frame.mirrorLoading) {
            const loadingRun=frame.mirrorLoadingRun;
            try{await frame.mirrorLoading;}catch(e){if(loadingRun===run)throw e;}
        }
        if(run!==serial || (frame.mirror && frame.mirrorLoaded))return;
        frame.mirrorLoadingRun=run;
        frame.mirrorLoading=(async()=>{
            frame.mirror?.remove();frame.mirror=makeElement(frame);frame.mirrorLoaded=false;
            await prepare(frame,frame.mirror,run);if(run===serial)frame.mirrorLoaded=true;
        })();
        try{await frame.mirrorLoading;}finally{frame.mirrorLoading=null;}
    }
    async function display(index,list,run,nav) {
        const requested=list[index];if(!requested)return;
        await ensureFrame(frames[requested.frame],run);
        if(!active(run,nav))return;
        index=locate(locationOf(requested),list);
        // Load only a section that may join the current spread, never the rest of the book.
        const first=list[index];
        if(spec.options.pageLayout===1 && !first.wide && !first.solo && list[index+1]?.pending) {
            await ensureFrame(frames[list[index+1].frame],run);if(!active(run,nav))return;
            index=locate(locationOf(requested),list);
        }
        // Offsets survive pane eviction. A known page is not necessarily loaded:
        // restore every pane in the spread, including an evicted second section.
        let panels;
        for(;;) {
            index=locate(locationOf(requested),list);panels=plan(index,list);
            const missing=[...new Set(panels.map(panel=>frames[list[panel.page].frame]))].filter(f=>!f.el || !f.loaded);
            if(!missing.length)break;
            for(const f of missing){await ensureFrame(f,run);if(!active(run,nav))return;}
        }
        const used=new Set();
        for(const panel of panels) {
            const f=frames[list[panel.page].frame];
            if(used.has(f)){await ensureMirror(f,run);if(!active(run,nav))return;}
            used.add(f);
        }
        if(!active(run,nav))return;
        for(const panel of displayed)panel.el.style.visibility='hidden';displayed=[];used.clear();
        for(const panel of panels) {
            const p=list[panel.page],f=frames[p.frame],el=used.has(f)?f.mirror:f.el;used.add(f);
            el.style.visibility='visible';el.contentWindow.scrollTo(f.vertical?0:p.local*W,f.vertical?p.local*H:0);displayed.push({...panel,el});
        }
        if(list===pages){current=index;note=-1;}else note=index;
        ready=true;fit();changed=true;
        if(controls)controls.style.display=history.length || note>=0?'flex':'none';
        // Keep current and previous spread panes, with page offsets retained.
        const keep=new Set(displayed.map(p=>list[p.page].frame));
        for(const f of frames)if(f.loaded && !f.loading && !f.mirrorLoading && !keep.has(f.index) && !f.recent) {
            f.el.remove();f.mirror?.remove();f.el=null;f.mirror=null;f.mirrorLoaded=false;f.nodes=[];f.loaded=false;
        }
        for(const f of frames)f.recent=used.has(f);
    }
    async function select(index,tail=false,nav=++navigation,run=serial) {
        if(!pages.length)return;
        index=Math.max(0,Math.min(pages.length-1,index));
        try {
            if(tail){await ensureFrame(frames[pages[index].frame],run);if(!active(run,nav))return;index=pages.length-1;}
            await display(index,pages,run,nav);
        }catch(e){if(active(run,nav))error=String(e.message || e);}
    }
    async function step(direction,onePage=false) {
        if(!pages.length)return;
        const here=locationOf(pages[current]);const run=serial,nav=++navigation;
        try {
            if(direction<0 && current>0 && pages[current-1].pending)await ensureFrame(frames[pages[current-1].frame],run);
            if(!active(run,nav))return;
            current=locate(here,pages);let target=current;
            if(direction>0)target=current+(onePage?1:plan(current,pages).length);
            else if(onePage)target=current-1;
            else {const candidate=Math.max(0,current-2);target=plan(candidate,pages).some(p=>p.page===current-1)?candidate:current-1;}
            if(target<0 || target>=pages.length || target===current){boundary=direction;return;}
            await select(target,false,nav,run);
        } catch(e){if(active(run,nav))error=String(e.message || e);}
    }
    async function showNote(index,nav=++navigation,run=serial) {
        try{await display(Math.max(0,Math.min(notes.length-1,index)),notes,run,nav);}catch(e){if(active(run,nav))error=String(e.message || e);}
    }
    async function start(value) {
        ++serial;++navigation;spec=value;messages=spec.messages || {};ready=false;error='';changed=false;revision=1;pages=[];notes=[];history=[];displayed=[];note=-1;current=0;
        inferredDirection=!!spec.rightToLeft;
        document.body.replaceChildren();document.body.style.cssText=`margin:0;overflow:hidden;background:${spec.options.background}`;
        frames=spec.sections.map((section,index)=>({section,index,el:null,mirror:null,loaded:false,loading:null,vertical:false}));
        for(const f of frames)(f.section.auxiliary?notes:pages).push({section:canonicalPath(f.section),offset:f.section.baseOffset || 0,frame:f.index,local:0,wide:false,solo:true,side:0,pending:true,rightToLeft:!!spec.rightToLeft});
        controls=document.createElement('div');controls.style.cssText='position:fixed;right:8px;top:8px;z-index:100;display:none;gap:6px';
        for(const [label,fn] of [[message('back'),back],[message('notePrevious'),()=>note>=0&&showNote(note-1)],[message('noteNext'),()=>note>=0&&showNote(note+1)]]){const button=document.createElement('button');button.textContent=label;button.onclick=fn;controls.append(button);}
        document.body.append(controls);
        const run=serial,nav=navigation;
        try{const location=spec.location || {};const target=pages[locate(location)];if(target)await ensureFrame(frames[target.frame],run);if(active(run,nav))await select(locate(location),false,nav,run);}
        catch(e){if(active(run,nav))error=String(e.message || e);}
    }
    async function navigate(location) {
        const candidate=pages[locate(location)];
        const f=(candidate?.section===location.section?frames[candidate.frame]:null) || frames.find(f=>f.section.path===location.section || canonicalPath(f.section)===location.section);if(!f)return;
        remember();const run=serial,nav=++navigation;try{await ensureFrame(f,run);if(!active(run,nav))return;
            const list=f.section.auxiliary?notes:pages;let index=locate(location,list);
            if(location.fragment){const el=f.el.contentDocument.getElementById(location.fragment);if(el){const range=f.el.contentDocument.createRange();range.selectNodeContents(el);const local=pageAt(f,range);const found=list.findIndex(p=>p.frame===f.index && p.local===local);if(found>=0)index=found;}}
            if(f.section.auxiliary)await showNote(index,nav,run);else await select(index,false,nav,run);
        }catch(e){if(active(run,nav))error=String(e.message || e);}
    }
    function back(){const old=history.pop();if(old)navigate(old.location);}
    addEventListener('resize',fit);
    return {start,select,step,last(){return select(pages.length-1,true);},navigate,back,
        pause(){++serial;++navigation;error='';},stepNote(direction){if(note>=0)showNote(note+direction);},
        state(includePages){const p=pages[current];const result={ready,error,current,changed,revision,boundary,complete:pages.every(p=>!p.pending),auxiliary:note>=0,noteSection:notes[note]?.section || '',vertical:!!frames[p?.frame]?.vertical,rightToLeft:!!p?.rightToLeft,visible:displayed.map(p=>p.page),pages:(typeof includePages==='number'?includePages!==revision:includePages)?pages:[]};changed=false;boundary=0;error='';return result;},
        async find(text){if(!text)return false;const run=serial,nav=++navigation;for(let n=0;n<frames.length;++n){if(!active(run,nav))return false;const f=frames[(frames[pages[current]?.frame]?.index+1+n)%frames.length];if(f.section.auxiliary)continue;await ensureFrame(f,run);if(!active(run,nav))return false;const at=(f.section.plain || '').toLocaleLowerCase().indexOf(text.toLocaleLowerCase());if(at>=0){await select(locate({section:canonicalPath(f.section),offset:at+(f.section.baseOffset || 0)}),false,nav,run);return true;}}return false;}
    };
})();
