// SPDX-License-Identifier: GPL-3.0-only
// Run the actual reader controller against a small deterministic DOM adapter.
// This tests scheduling and navigation; native WebEngine tests cover painting.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const panels=fs.readFileSync(path.join(__dirname,'../src/documentpanels.js'),'utf8');
const reader=fs.readFileSync(path.join(__dirname,'../src/documentlayout.js'),'utf8');
function environment(fixtures) {
    const loaded=[];
    class Element {
        constructor(tag){this.tagName=tag.toUpperCase();this.nodeType=1;this.children=[];this.style={};this.dataset={};this.parentElement=null;}
        get childNodes(){return this.children;}
        append(...items){for(const i of items){this.children.push(i);i.parentElement=this;}}
        replaceChildren(){this.children=[];}
        remove(){if(this.parentElement)this.parentElement.children=this.parentElement.children.filter(c=>c!==this);}
        replaceWith(e){const p=this.parentElement;if(p){p.children[p.children.indexOf(this)]=e;e.parentElement=p;}}
        matches(selector){return selector.split(',').includes(this.tagName.toLowerCase());}
        hasAttribute(name){return name==='data-koma-offset' && this.dataset.komaOffset!==undefined;}
        getAttribute(name){return name==='data-koma-offset'?this.dataset.komaOffset:undefined;}
        querySelectorAll(){return [];}
        closest(){return null;}
        set textContent(v){this._text=v;}get textContent(){return this._text || '';}
        get offsetWidth(){return parseFloat(this.style.width)||1200;}get offsetHeight(){return parseFloat(this.style.height)||1600;}
    }
    function content(fixture) {
        const body=new Element('body'),root=new Element('html'),head=new Element('head');
        const text=fixture.text ?? 'abcdefghij'.repeat(fixture.pages || 1);
        if(!fixture.image)body.append({nodeType:3,textContent:text,length:text.length,parentElement:body});
        const images=[];
        if(fixture.image){const image=new Element('img');image.naturalWidth=fixture.image[0];image.naturalHeight=fixture.image[1];image.decode=()=>Promise.resolve();images.push(image);body.append(image);}
        root.scrollWidth=(fixture.pages || 1)*1200;root.scrollHeight=(fixture.pages || 1)*1600;
        const metadata={error:fixture.error || '',fixed:!!fixture.fixed,authoredFixed:!!fixture.authoredFixed,imagePage:!!fixture.image,cover:!!fixture.cover,wide:false,side:fixture.side || 0,solo:!!fixture.solo,width:1200,height:1600};
        let mode=fixture.mode || 'horizontal-tb';
        head.querySelector=()=>({content:JSON.stringify(metadata)});
        head.append=el=>{if(el.textContent.includes('writing-mode:vertical-rl'))mode='vertical-rl';if(el.textContent.includes('writing-mode:horizontal-tb'))mode='horizontal-tb';};
        return {body,head,createElement:tag=>new Element(tag),documentElement:root,images,fonts:{ready:Promise.resolve()},querySelectorAll:()=>[],addEventListener(){},get mode(){return mode;},
            getElementById(){return null;},createRange(){let offset=0;return {setStart(_,i){offset=i;},setEnd(){},selectNodeContents(){},getBoundingClientRect(){return {left:Math.floor(offset/10)*1200,top:Math.floor(offset/10)*1600};}};}};
    }
    const document={body:new Element('body'),createElement(tag){
        const el=new Element(tag);
        if(tag==='iframe') {
            Object.defineProperty(el,'src',{set(value){const key=decodeURIComponent(value.slice(value.lastIndexOf('/')+1));loaded.push(key);const fixture=fixtures[key];assert.ok(fixture,`unexpected source ${key}`);el.contentDocument=content(fixture);el.contentWindow={scrollX:0,scrollY:0,scrollTo(x,y){this.scrollX=x;this.scrollY=y;}};const count=loaded.filter(p=>p===key).length,delay=fixture.delays?.[count-1] ?? fixture.delay;const finish=()=>fixture.failLoads?.includes(count)?el.onerror?.():el.onload?.();if(delay)setTimeout(finish,delay);else setImmediate(finish);}});
        }
        return el;
    }};
    const sandbox={window:{},document,Node:{ELEMENT_NODE:1,TEXT_NODE:3},innerWidth:800,innerHeight:1200,
        setTimeout(fn,ms){const t=setTimeout(fn,ms);t.unref();return t;},clearTimeout,requestAnimationFrame:fn=>setImmediate(fn),addEventListener(){}};
    // Find the content document owning a body so computed style follows injected CSS.
    sandbox.getComputedStyle=body=>{
        const frame=document.body.children.find(f=>f.contentDocument?.body===body);
        return {writingMode:frame?.contentDocument.mode || 'horizontal-tb'};
    };
    vm.createContext(sandbox);vm.runInContext(panels,sandbox);vm.runInContext(reader,sandbox);
    const api=sandbox.window.komaDocument;
    const options={pageLayout:1,wideImages:true,coverAlone:true,publisherStyles:true,ruby:true,size:22,spacing:8,margin:60,background:'#ffffff',foreground:'#000000',writingMode:0};
    return {api,loaded,sandbox,spec(extra={}){return {base:'koma-document://fixture/',epub:true,directionSpecified:true,rightToLeft:false,sections:Object.keys(fixtures).map(path=>({path,plain:'',baseOffset:0})),options:{...options,...extra}};}};
}
(async()=>{
    {
        const e=environment({'cover.xhtml':{image:[600,800],cover:true},'body.xhtml':{pages:3},'later.xhtml':{pages:2}});
        await e.api.start(e.spec());let s=e.api.state(true);
        assert.equal(s.error,'');assert.equal(s.ready,true);assert.equal(s.visible.length,1);assert.deepEqual(e.loaded,['cover.xhtml']);assert.equal(s.complete,false);
        const frame=e.sandbox.document.body.children.find(f=>f.contentDocument);
        assert.ok(frame.style.transform.startsWith('scale(0.666'),frame.style.transform);
        await e.api.step(1);s=e.api.state(true);assert.equal(s.current,1);assert.equal(s.visible.length,2);assert.equal(e.loaded.includes('later.xhtml'),false);
        await e.api.step(-1);s=e.api.state(true);assert.equal(s.current,0);assert.equal(s.visible.length,1);
        await e.api.last();s=e.api.state(true);assert.equal(s.current,s.pages.length-1);assert.equal(s.error,'');
        await e.api.step(-1);assert.equal(e.api.state(true).error,'');
    }
    {
        const e=environment({'body.xhtml':{pages:5},'other.xhtml':{pages:2}});const spec=e.spec();spec.location={section:'body.xhtml',offset:30};
        await e.api.start(spec);const s=e.api.state(true);assert.equal(s.pages[s.current].offset,30);assert.equal(e.loaded.includes('other.xhtml'),false);
    }
    {
        const e=environment({'previous.xhtml':{pages:3},'current.xhtml':{pages:2}}),spec=e.spec();spec.location={section:'current.xhtml',offset:0};
        await e.api.start(spec);assert.equal(e.loaded.includes('previous.xhtml'),false);
        await e.api.step(-1);const s=e.api.state(true);assert.equal(s.pages[s.current].section,'previous.xhtml');assert.equal(s.pages[s.current].offset,10);assert.equal(s.visible.length,2);
    }
    for(const [published,mode,expected] of [[false,2,true],[true,1,false]]) {
        const e=environment({'body.xhtml':{pages:3}}),spec=e.spec({writingMode:mode});spec.rightToLeft=published;
        await e.api.start(spec);const s=e.api.state(true);assert.equal(s.rightToLeft,expected);assert.equal(s.visible.length,2);
    }
    {
        const e=environment({'fixed.xhtml':{pages:1,fixed:true,authoredFixed:true,mode:'vertical-rl'},'other.xhtml':{pages:1,fixed:true}}),spec=e.spec({writingMode:1});spec.rightToLeft=true;
        await e.api.start(spec);assert.equal(e.api.state(true).rightToLeft,true);
    }
    {
        const e=environment({'text.html':{pages:2},'picture.html':{image:[1536,768]},'after.html':{pages:2}}),spec=e.spec();spec.epub=false;spec.sections[1].locationPath='text.html';spec.sections[1].baseOffset=20;spec.sections[2].locationPath='text.html';spec.sections[2].baseOffset=30;
        await e.api.start(spec);await e.api.step(1);const s=e.api.state(true);assert.equal(s.visible.length,1);assert.equal(s.pages[s.current].wide,true);
    }
    {
        const e=environment({'good.xhtml':{pages:1},'bad.xhtml':{error:'invalid chapter'},'next.xhtml':{pages:1}});
        await e.api.start(e.spec({pageLayout:0}));await e.api.select(1);assert.equal(e.api.state(true).error,'invalid chapter');
        assert.equal(e.api.state(true).error,'');await e.api.select(2);assert.equal(e.api.state(true).error,'');assert.equal(e.api.state(true).ready,true);
    }
    {
        const e=environment({'slow.xhtml':{pages:2,delay:20},'latest.xhtml':{pages:2}});
        const first=e.api.start(e.spec({pageLayout:0}));await e.api.select(1);await first;
        const s=e.api.state(true);assert.equal(s.pages[s.current].section,'latest.xhtml');
    }
    {
        const fixtures=Object.fromEntries(Array.from({length:6},(_,i)=>[`${i}.xhtml`,{pages:1}]));
        const e=environment(fixtures);await e.api.start(e.spec());
        await e.api.select(2);await e.api.select(4);await e.api.select(0);
        const s=e.api.state(true);
        assert.equal(s.error,'','both pages of an evicted spread must be reloaded');
        assert.deepEqual(Array.from(s.visible),[0,1]);
        assert.equal(e.loaded.filter(p=>p==='1.xhtml').length,2);
        const visible=e.sandbox.document.body.children.filter(f=>f.contentDocument && f.style.visibility==='visible');
        assert.equal(visible.length,2);
    }
    {
        const e=environment({'good.xhtml':{pages:1},'pending.xhtml':{pages:2,delay:20}});await e.api.start(e.spec({pageLayout:0}));
        const pending=e.api.select(1);e.api.pause();await pending;
        assert.equal(e.api.state(true).current,0);await e.api.select(1);
        const s=e.api.state(true);assert.equal(s.error,'');assert.equal(s.pages[s.current].section,'pending.xhtml');
        assert.equal(e.loaded.filter(p=>p==='pending.xhtml').length,2);
    }
    {
        const e=environment({'previous.xhtml':{pages:3,delay:20},'current.xhtml':{pages:2},'latest.xhtml':{pages:2}}),spec=e.spec({pageLayout:0});
        spec.location={section:'current.xhtml',offset:0};await e.api.start(spec);
        const old=e.api.step(-1);await e.api.select(3);await old;
        const s=e.api.state(true);assert.equal(s.error,'');assert.equal(s.pages[s.current].section,'latest.xhtml','a delayed previous-page request must not replace a newer move');
    }
    {
        const e=environment({'body.xhtml':{pages:4,delays:[0,30]}});
        const first=e.api.start(e.spec());
        while(e.loaded.length<2)await new Promise(resolve=>setImmediate(resolve));
        await e.api.select(1);
        let s=e.api.state(true);assert.equal(s.error,'');assert.equal(s.current,1);
        const visible=e.sandbox.document.body.children.filter(f=>f.contentDocument && f.style.visibility==='visible');
        assert.equal(visible.length,2);assert.ok(visible.every(f=>f.style.width),'a shared mirror must finish preparation before it becomes visible');
        await first;s=e.api.state(true);assert.equal(s.current,1);
        assert.equal(e.loaded.length,2,'concurrent moves must share one prepared mirror');
    }
    for(const failLoads of [[],[1]]) {
        const e=environment({'good.xhtml':{pages:1},'pending.xhtml':{pages:2,delay:20,failLoads}});await e.api.start(e.spec({pageLayout:0}));
        const old=e.api.select(1);e.api.pause();const first=e.api.select(1),latest=e.api.select(1);
        await Promise.all([old,first,latest]);const s=e.api.state(true);
        assert.equal(s.error,'');assert.equal(s.pages[s.current].section,'pending.xhtml');
        assert.equal(e.loaded.filter(p=>p==='pending.xhtml').length,2,'resume must share the replacement pane after an obsolete load');
    }
    for(const operation of ['navigate','find']) {
        const e=environment({'current.xhtml':{pages:1},'slow.xhtml':{pages:1,delay:20,text:'needle'},'latest.xhtml':{pages:1}});
        await e.api.start(e.spec({pageLayout:0}));
        const old=operation==='navigate'?e.api.navigate({section:'slow.xhtml',offset:0}):e.api.find('needle');
        await e.api.select(2);await old;
        const s=e.api.state(true);assert.equal(s.error,'');assert.equal(s.pages[s.current].section,'latest.xhtml',`a delayed ${operation} must not replace a newer move`);
    }
    for(const rightToLeft of [false,true]) {
        const fixtures=Object.fromEntries(Array.from({length:9},(_,i)=>[`${i}.xhtml`,{pages:i%3+1,delay:i%2?2:0}]));
        const e=environment(fixtures),spec=e.spec();spec.rightToLeft=rightToLeft;await e.api.start(spec);
        for(let n=0;n<20;++n) {
            const moves=[e.api.step(1),e.api.step(-1),e.api.last(),e.api.select(n%3)];
            await Promise.all(moves);
            let s=e.api.state(true);assert.equal(s.error,'');
            assert.equal(s.current,n%3,'latest move must win a rapid burst');
            assert.ok(s.visible.length>=1 && s.visible.length<=2);
            const at=s.current;await e.api.step(1);s=e.api.state(true);assert.equal(s.error,'');assert.ok(s.current>at,'navigation must remain usable after a burst');
            await e.api.step(-1);assert.equal(e.api.state(true).error,'');
        }
    }
    console.log('document_layout_tests: lazy spreads, rapid navigation, shared mirrors, cancellation, cover fit, direction and resume passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
