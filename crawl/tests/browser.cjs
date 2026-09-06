// Isolated browser regressions: every response is local fixture data.
const {chromium,expect}=require('playwright/test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const M=require('../web/model.js');
const ROOT=path.resolve(__dirname,'..');
const SITE=path.resolve(ROOT,process.env.MAGPIE_TEST_SITE||'web');
const old={id:'test:barrel',t:'Barrel',s:'test',u:'https://source.test/barrel',ty:'3d_model',st:'lowpoly',l:'cc_by',lu:'https://creativecommons.org/licenses/by/4.0/',a:'',tg:'wood',c:1,n:0,p:5};
const current={...old,a:'Ada'};
const sa={...current,id:'test:stone',t:'Stone wall',u:'https://source.test/wall',l:'cc_by_sa',lu:'https://creativecommons.org/licenses/by-sa/4.0/',tg:'stone',p:10};
const cc0={...current,id:'test:lamp',t:'Lamp',u:'https://source.test/lamp',l:'cc0',lu:'https://creativecommons.org/publicdomain/zero/1.0/',n:1};
const fixture={generated:1700000000,assets:[current,sa,cc0],sources:[{name:'test',site:'https://source.test',assets:3,commercial:3,dead:0,last_seen:1600000000,last_success:0,crawl_state:3}],stats:{total:3,commercial:3,nocredit:1,unclassifiable:0,dead:0,unchecked:3}};
let browser;
async function scenario(name,body,{data=fixture,saved,filtersOpen=true}={}){
  const context=await browser.newContext({acceptDownloads:true});
  const errors=[];
  try{
    await context.route('**/*',route=>{
      const url=new URL(route.request().url());
      if(url.hostname!=='magpie.test') return route.abort();
      const file=url.pathname==='/model.js'?'model.js':url.pathname==='/data.json'?null:'index.html';
      return route.fulfill({status:200,contentType:file==='index.html'?'text/html':file?'application/javascript':'application/json',body:file?fs.readFileSync(path.join(SITE,file)):JSON.stringify(data)});
    });
    if(saved) await context.addInitScript(value=>localStorage.setItem('magpie.basket.v1',JSON.stringify(value)),saved);
    if(filtersOpen) await context.addInitScript(()=>document.addEventListener('DOMContentLoaded',()=>{document.querySelector('#filters').open=true;document.querySelector('#morefilters').open=true;}));
    const page=await context.newPage();
    page.on('pageerror',error=>errors.push(error.message));
    await page.goto('http://magpie.test/');
    await body(page,context);
    assert.deepEqual(errors,[]);
    console.log('PASS '+name);
  }finally{await context.close();}
}
(async()=>{
  const {publication}=require('../publish.cjs');
  const blocked={...current,s:'opengameart',th:'https://opengameart.org/preview.png'};
  assert.equal(M.previewAllowed(blocked),false);
  assert.equal(M.previewAllowed({...blocked,s:'kenney'}),false);
  assert.equal(M.previewAllowed({...current,s:'polyhaven',th:'https://cdn.polyhaven.com/asset_img/thumbs/wood.png'}),true);
  assert.equal(M.previewAllowed({...current,s:'polyhaven',th:'https://cdn.polyhaven.com/example.png'}),false);
  assert.equal(M.previewAllowed({...current,s:'gameicons',a:'',th:'https://cdn.jsdelivr.net/gh/game-icons/icons@master/a.svg'}),false);
  const staged=publication({...fixture,assets:[blocked,sa,cc0]},{version:1,asset_ids:[sa.id],source_urls:[cc0.u]});
  assert.equal(staged.assets.length,1); assert.equal(staged.stats.total,1);
  assert.equal(staged.assets[0].th,undefined);
  assert.throws(()=>publication(fixture,{version:1,asset_ids:[''],source_urls:[]}));
  const notes={...old,credit_author:'Project creator',credit_notice:'Retain this notice',modifications:'Unmodified',evidence_url:old.u,reviewed_at:1700000000000};
  const revised=M.readProject(M.project([M.accept(notes,current)]))[0];
  assert.equal(revised.credit_notice,notes.credit_notice); assert.equal(revised.reviewed_at,0);
  assert.equal(revised.history[0].reviewed_at,notes.reviewed_at);
  assert.throws(()=>M.saved({...current,reviewed_at:1e20}),/Invalid save date/);
  assert.equal(M.matchesQuery('concrete road barrier',['road','barriers']),true);
  assert.equal(M.matchesQuery('box bush church class berry',['boxes','bushes','churches','classes','berries']),true);
  assert.equal(M.matchesQuery('gla gra statu',['glass']),false);
  assert.equal(M.matchesQuery('barr',['barriers']),false);
  console.log('PASS publication exclusions, preview policy and review history');
  browser=await chromium.launch({channel:process.env.MAGPIE_BROWSER_CHANNEL||(process.platform==='win32'?'msedge':undefined),headless:true});
  try{
    await scenario('collapsed filters leave results visible on desktop and mobile',async page=>{
      for(const viewport of [{width:1280,height:800},{width:390,height:844}]){
        await page.setViewportSize(viewport);
        await expect(page.locator('#results .hit')).toHaveCount(3);
        await expect(page.locator('#filters')).not.toHaveAttribute('open','');
        const box=await page.locator('#results').boundingBox();
        assert.ok(box.y<240,`Grid starts at ${box.y}`);
        assert.ok((await page.locator('#q').boundingBox()).width>viewport.width*.85);
        assert.ok(box.width>viewport.width*.9,`Grid width ${box.width}`);
        assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
        await page.locator('#filters>summary').press('Space');
        await page.click('[data-use="nocredit"]');
        await expect(page.locator('#results .hit')).toHaveCount(1);
        await page.locator('#filters>summary').click();
        await expect(page.locator('#chips')).toContainText('reported CC0');
        await page.locator('[data-chip="use"]').click();
        await expect(page.locator('#results .hit')).toHaveCount(3);
        fs.mkdirSync(path.join(ROOT,'output/playwright'),{recursive:true});
        await page.screenshot({path:path.join(ROOT,`output/playwright/compact-${viewport.width}.png`)});
      }
    },{filtersOpen:false});
    await scenario('clear credits is undoable after reload and preserves later additions',async page=>{
      await page.locator('#projectfile').setInputFiles({name:'project.json',mimeType:'application/json',buffer:Buffer.from(M.project([{...current,credit_notice:'Keep my notice',history:[old]}]))});
      await page.click('#creditactions>summary');await page.click('#trayclear');
      await expect(page.locator('#undoclear')).toBeVisible();
      await expect(page.locator('#tray')).not.toBeVisible();
      await page.reload();
      await page.locator(`[data-pick="${cc0.u}"]`).click();
      await page.click('#restorecredits');
      await expect(page.locator('#traycount')).toHaveText('2 assets in credits');
      const saved=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1')));
      assert.equal(saved.find(a=>a.id===current.id).credit_notice,'Keep my notice');
      assert.equal(saved.find(a=>a.id===current.id).history.length,1);
      await expect(page.locator('#undoclear')).not.toBeVisible();
      await page.reload();await expect(page.locator('#traycount')).toHaveText('2 assets in credits');
    });
    await scenario('clear keeps the list when the undo copy cannot be saved',async page=>{
      await page.locator(`[data-pick="${current.u}"]`).click();
      await page.evaluate(()=>{Storage.prototype.setItem=function(){throw new DOMException('Full','QuotaExceededError');};});
      await page.click('#creditactions>summary');await page.click('#trayclear');
      await expect(page.locator('#traycount')).toHaveText('1 asset in credits');
      await expect(page.locator('#projectnote')).toContainText('Your credits were kept');
      await expect(page.locator('#undoclear')).not.toBeVisible();
    });
    await scenario('small phone tray and everyday filters stay compact',async page=>{
      await page.setViewportSize({width:320,height:640});
      await page.locator(`[data-pick="${current.u}"]`).click();
      assert.ok((await page.locator('#tray').boundingBox()).height<70);
      await page.click('#filters>summary');
      await expect(page.locator('#sourcefilter')).toBeVisible();
      await expect(page.locator('#morefilters')).not.toHaveAttribute('open','');
      await page.selectOption('#sourcefilter','test');
      await page.click('#showresults');
      await expect(page.locator('#filters')).not.toHaveAttribute('open','');
      await page.reload();await page.click('#filters>summary');
      await expect(page.locator('#sourcefilter')).toHaveValue('test');
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
      await page.click('#showresults');await page.click('#creditactions>summary');
      await expect(page.locator('#projectexport')).toBeVisible();
      await expect(page.locator('#trayclear')).toBeVisible();
      await page.click('#creditactions>summary');
      await page.screenshot({path:path.join(ROOT,'output/playwright/simple-320.png')});
    },{filtersOpen:false});
    await scenario('common plurals match without dropping filters or source-only mode',async page=>{
      await page.fill('#q','barrels');await expect(page.locator('#results .hit')).toHaveCount(1);
      await page.click('[data-use="nocredit"]');await expect(page.locator('#results .hit')).toHaveCount(0);
      await expect(page.locator('#results')).toContainText('Dropping the licence filter');
      await page.click('[data-use="any"]');await page.uncheck('#includeinferred');
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await page.fill('#q','nonexistent words');await expect(page.locator('#results')).toContainText('Try fewer words');
    });
    await scenario('legacy OGA-BY records disclose the unknown version in details and credits',async page=>{
      await page.locator('.hit').first().press('Enter');
      await expect(page.locator('#dbody')).toContainText('Not verified in this legacy record');
      await page.click('#dpick');await page.click('#dclose');await page.click('#trayopen');
      await expect(page.locator('#credits')).toHaveValue(/exact OGA-BY version not recorded/);
    },{data:{...fixture,assets:[{...current,l:'oga_by',lu:'https://opengameart.org/content/faq'}]}});
    await scenario('failed storage never reports a successful import or notes save and remains exportable',async page=>{
      await page.evaluate(()=>{window.realSetItem=Storage.prototype.setItem;Storage.prototype.setItem=function(){throw new DOMException('Full','QuotaExceededError');};});
      await page.locator('#projectfile').setInputFiles({name:'project.json',mimeType:'application/json',buffer:Buffer.from(M.project([current]))});
      await expect(page.locator('#projectnote')).toContainText('only in this tab');
      await expect(page.locator('#projectnote')).not.toContainText('Project imported');
      await page.click('#trayopen');
      await expect(page.locator('#dlgnote')).toContainText('browser storage failed');
      await page.locator('#crediteditor summary').click();
      await page.fill('#creditnotice','Keep this notice');await page.click('#creditsavenotes');
      await expect(page.locator('#creditnotesstatus')).toContainText('browser storage failed');
      await expect(page.locator('#creditnotesstatus')).not.toHaveText('Notes saved');
      await page.click('#dlgclose');
      const downloaded=page.waitForEvent('download');await page.locator('#creditactions>summary').click();await page.click('#projectexport');
      const file=await downloaded;
      assert.equal(M.readProject(fs.readFileSync(await file.path(),'utf8'))[0].credit_notice,'Keep this notice');
      assert.equal(await page.evaluate(()=>localStorage.getItem('magpie.basket.v1')),null);
      await page.evaluate(()=>{Storage.prototype.setItem=window.realSetItem;});
      await page.click('#trayopen');await page.click('#creditsavenotes');
      await expect(page.locator('#creditnotesstatus')).toHaveText('Notes saved');
      await expect(page.locator('#dlgnote')).not.toContainText('browser storage failed');
      await page.reload();await page.click('#trayopen');
      await expect(page.locator('#credits')).toHaveValue(/Keep this notice/);
    });
    await scenario('accepting removed source fields clears them while retaining notes and history',async page=>{
      await page.click('#trayopen');await page.click('[data-accept]');
      await expect(page.locator('[data-accept]')).toHaveCount(0);
      const result=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      assert.equal(result.a,undefined);assert.equal(result.lu,undefined);assert.equal(result.th,undefined);
      assert.equal(result.credit_notice,'Keep');assert.equal(result.history[0].a,'Ada');
      assert.equal(result.history[0].lu,current.lu);assert.equal(result.reviewed_at,0);
    },{saved:[{...current,th:'https://source.test/old.png',credit_notice:'Keep',reviewed_at:123}],data:{...fixture,assets:[{...current,a:null,lu:null,th:null}]}});
    await scenario('revision 101 and previously oversized histories survive reload and backup',async page=>{
      const saved={...old,history:Array.from({length:101},(_,i)=>({...old,t:'Revision '+i}))};
      await page.locator('#projectfile').setInputFiles({name:'project.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify([saved]))});
      await page.click('#trayopen');await page.click('[data-accept]');
      await page.reload();await page.click('#trayopen');
      await expect(page.locator('#credits')).toHaveValue(/by Ada/);
      const result=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      const restored=M.readProject(M.project([result]))[0];
      assert.equal(restored.history.length,102);assert.equal(restored.history[0].t,'Revision 0');
      assert.equal(restored.history[101].t,old.t);
    });
    await scenario('inferred tags can be excluded from search and shared tag filters',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(3);
      await page.fill('#q','metal');
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await page.uncheck('#includeinferred');
      await expect(page.locator('#results')).toContainText('No matches');
      await page.reload();
      await expect(page.locator('#includeinferred')).not.toBeChecked();
      await expect(page.locator('#results')).toContainText('No matches');
      await page.fill('#q','wood');
      await expect(page.locator('#results .hit')).toHaveCount(2);
      await page.click('#clearq');
      await expect(page.locator('#f-tag [data-tag="metal"]')).toHaveCount(0);
      await page.check('#includeinferred');
      await expect(page.locator('#f-tag')).toContainText('1 inferred');
      await page.check('#f-tag [data-tag="metal"]');
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await page.uncheck('#includeinferred');
      await expect(page.locator('#results')).toContainText('No matches');
      await page.locator('[data-chip="inferred"]').click();
      await expect(page.locator('#results .hit')).toHaveCount(1);
    },{data:{...fixture,assets:[{...current,tga:'metal'},sa,cc0],}});
    await scenario('missing evidence stays unknown and commercial labels make no clearance claim',async page=>{
      await expect(page.locator('#health')).toContainText('not recorded');
      await expect(page.locator('#sources')).toContainText('match commercial filter');
      await page.click('[data-use="commercial"]');
      await expect(page.locator('#chips')).toContainText('review terms');
      await expect(page.locator('#chips')).not.toContainText('can ship commercially');
      await page.locator('.hit').first().press('Enter');
      await expect(page.locator('#dbody')).toContainText('Reported licence');
      await expect(page.locator('#dbody')).toContainText('Source tags: not recorded');
      await expect(page.locator('#dbody')).toContainText('Link checked');
      await expect(page.locator('#dbody')).toContainText('not recorded');
      assert.equal(await page.evaluate(()=>dateLabel(1e20)),'not recorded');
      assert.equal(await page.evaluate(()=>dateLabel(-1)),'not recorded');
    },{data:{...fixture,assets:[{...current,tg:'',seen:0,checked:0}]}});
    await scenario('project attribution notes persist and reject unsafe evidence URLs',async page=>{
      await page.locator('.hit[data-u="https://source.test/barrel"] [data-pick]').click();
      await page.click('#trayopen');
      await page.locator('#crediteditor summary').click();
      await page.fill('#creditauthor','Project creator');
      await page.fill('#creditnotice','Retain this copyright notice');
      await page.fill('#creditmods','Unmodified');
      await page.fill('#creditevidence','javascript:alert(1)');
      await page.click('#creditsavenotes');
      await expect(page.locator('#creditnotesstatus')).toContainText('HTTP(S)');
      await page.fill('#creditevidence','https://source.test/terms');
      await page.check('#creditreviewed');
      await page.click('#creditsavenotes');
      await expect(page.locator('#credits')).toHaveValue(/Retain this copyright notice/);
      fs.mkdirSync(path.join(ROOT,'output/playwright'),{recursive:true});
      await page.screenshot({path:path.join(ROOT,'output/playwright/credits.png')});
      await page.reload(); await page.click('#trayopen');
      await expect(page.locator('#credits')).toHaveValue(/Project creator/);
      await expect(page.locator('#credits')).toHaveValue(/Changes: Unmodified/);
      const saved=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      assert.ok(saved.reviewed_at); assert.equal(saved.evidence_url,'https://source.test/terms');
    });
    await scenario('restricted source previews never become image requests',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await expect(page.locator('#results img')).toHaveCount(0);
      await page.locator('.hit').press('Enter');
      await expect(page.locator('#dbody img')).toHaveCount(0);
    },{data:{...fixture,assets:[blocked]}});
    await scenario('saved credits detect enrichment and preserve original revision',async page=>{
      await expect(page.locator('#traysub')).toContainText('need review');
      await page.click('#trayopen');
      await expect(page.locator('#creditchanges')).toContainText('Ada');
      await expect(page.locator('#credits')).toHaveValue(/AUTHOR UNKNOWN/);
      await page.click('[data-accept]');
      await expect(page.locator('#credits')).toHaveValue(/by Ada/);
      const record=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      assert.equal(record.a,'Ada'); assert.equal(record.history[0].a,'');
      const download=page.waitForEvent('download');
      await page.click('#dlgclose'); await page.locator('#creditactions>summary').click();await page.click('#projectexport');
      const file=await download;
      const restored=M.readProject(fs.readFileSync(await file.path(),'utf8'));
      assert.equal(restored[0].history[0].a,'');

    },{saved:[old]});
    await scenario('saved CC0 licence is retained and changed terms are flagged',async page=>{
      await page.click('#trayopen');
      await expect(page.locator('#creditchanges')).toContainText('l');
      await expect(page.locator('#credits')).toHaveValue(/Saved records to review/);
      const record=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      assert.equal(record.l,'cc0');
      await page.click('[data-accept]');
      const revised=await page.evaluate(()=>JSON.parse(localStorage.getItem('magpie.basket.v1'))[0]);
      assert.equal(revised.history[0].l,'cc0');
      assert.equal(revised.l,'cc_by');
    },{saved:[{...old,l:'cc0',n:1}]});
    await scenario('stable identity survives a changed source URL',async page=>{
      await expect(page.locator('.hit.picked')).toHaveCount(1);
      await page.locator('.hit.picked [data-pick]').click();
      assert.equal(await page.evaluate(()=>basket.size),0);
    },{saved:[{...old,u:'https://source.test/old-barrel'}]});
    await scenario('project import restores history and rejects malformed backup atomically',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(3);
      const backup=M.project([M.accept(old,current)]);
      await page.locator('#projectfile').setInputFiles({name:'project.json',mimeType:'application/json',buffer:Buffer.from(backup)});
      await expect(page.locator('#traycount')).toHaveText('1 asset in credits');
      await page.locator('#projectfile').setInputFiles({name:'bad.json',mimeType:'application/json',buffer:Buffer.from('{"format":"bad"}')});
      await expect(page.locator('#projectnote')).toHaveText('Invalid project backup');
      await expect(page.locator('#traycount')).toHaveText('1 asset in credits');
      await page.reload();
      await expect(page.locator('#traycount')).toHaveText('1 asset in credits');
    });
    await scenario('search and facets preserve result semantics',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(3);
      await page.fill('#q','stone wall');
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await expect(page.locator('#results')).toContainText('Stone wall');
      await page.click('[data-use="nocredit"]');
      await expect(page.locator('#results')).toContainText('No matches');
      await page.click('#clearq');
      await expect(page.locator('#results .hit')).toHaveCount(1);
      await expect(page.locator('#results')).toContainText('Lamp');
    });
    await scenario('licence obligations appear in details and credits',async page=>{
      await page.locator('.hit[data-u="https://source.test/wall"]').press('Enter');
      await expect(page.locator('#dbody')).toContainText('ShareAlike');
      await page.click('#dpick'); await page.click('#dclose'); await page.click('#trayopen');
      await expect(page.locator('#credits')).toHaveValue(/share adaptations/);
    });
    await scenario('dialog keyboard input does not change background grid or basket',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(3);
      await page.locator('.hit').first().press('Enter');
      await page.keyboard.press('a'); await page.keyboard.press('ArrowDown');
      assert.equal(await page.evaluate(()=>basket.size),0);
      await expect(page.locator('.hit.cursor')).toHaveCount(0);
      await page.keyboard.press('Escape');
      await expect(page.locator('#detail')).not.toBeVisible();
    });
    await scenario('source freshness is distinct from export generation',async page=>{
      await expect(page.locator('#sources')).toContainText('2020-09-13');
      await expect(page.locator('#sources')).toContainText('incomplete');
      await expect(page.locator('#health')).toContainText('Export generated 2023-11-14');
    });
    await scenario('mobile controls and source health fit the viewport',async page=>{
      await page.setViewportSize({width:390,height:844});
      await expect(page.locator('#results .hit')).toHaveCount(3);
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
      await page.locator('#catalogueinfo>summary').click();await page.locator('#sources').scrollIntoViewIfNeeded();
      fs.mkdirSync(path.join(ROOT,'output/playwright'),{recursive:true});
      await page.screenshot({path:path.join(ROOT,'output/playwright/mobile.png')});
    });
    await scenario('malformed index fails visibly without rendering records',async page=>{
      await expect(page.locator('#status')).toContainText('Invalid asset record');
      await expect(page.locator('.hit')).toHaveCount(0);
    },{data:{...fixture,assets:[{...current,u:'javascript:alert(1)'}]}});
    await scenario('current full catalogue renders and searches',async page=>{
      await expect(page.locator('#results .hit')).toHaveCount(48);
      fs.mkdirSync(path.join(ROOT,'output/playwright'),{recursive:true});
      await page.screenshot({path:path.join(ROOT,'output/playwright/catalogue.png')});
      const times=await page.evaluate(()=>{
        const values=[]; for(const q of ['', 'a','dungeon','stone wall','road barrier','road barriers']){
          state.q=q; const start=performance.now();run();values.push({query:q,ms:Math.round(performance.now()-start),results:view.length});
        }return values;
      });
      assert.equal(times.find(t=>t.query==='road barriers').results,times.find(t=>t.query==='road barrier').results);
      console.log('Browser search including rendering: '+JSON.stringify(times));
    },{data:JSON.parse(fs.readFileSync(path.join(SITE,'data.json'),'utf8'))});
  }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
