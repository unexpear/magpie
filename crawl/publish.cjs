// Stage only public files. Exclusions apply on every deployment, including recrawls.
const fs=require('node:fs');
const path=require('node:path');
const M=require('./web/model.js');
function publication(data,rules){
  M.validateIndex(data);
  if(rules?.version!==1 || !Array.isArray(rules.asset_ids) || !Array.isArray(rules.source_urls) ||
     ![...rules.asset_ids,...rules.source_urls].every(x=>typeof x==='string' && x.trim()))
    throw new Error('Invalid publication exclusions');
  const ids=new Set(rules.asset_ids),urls=new Set(rules.source_urls);
  const assets=data.assets.filter(a=>!ids.has(a.id) && !urls.has(a.u)).map(a=>{
    const out=M.asset(a); if(!M.previewAllowed(out)) delete out.th; return out;
  });
  const sources=data.sources.map(source=>{
    const rows=assets.filter(a=>a.s===source.name);
    return {...source,assets:rows.length,commercial:rows.filter(a=>a.c).length,
      source_tagged:rows.filter(a=>a.tg).length,missing_authors:rows.filter(a=>!a.n && !a.a).length};
  }).filter(s=>s.assets);
  const stats={...data.stats,total:assets.length,commercial:assets.filter(a=>a.c).length,
    nocredit:assets.filter(a=>a.n).length,unclassifiable:assets.filter(a=>a.ty==='unclassifiable').length,
    source_tagged:assets.filter(a=>a.tg).length,auto_only:assets.filter(a=>!a.tg && a.tga).length,
    unchecked:assets.filter(a=>!a.checked).length};
  return M.validateIndex({...data,assets,sources,stats});
}
function checkSize(data,bytes){
  if(data.assets.length>50000 || bytes>25*1024*1024)
    throw new Error('Publication stopped: 50,000 assets or 25 MiB site limit exceeded');
}
if(require.main===module){
  const data=JSON.parse(fs.readFileSync(path.join(__dirname,'web/data.json'),'utf8'));
  const rules=JSON.parse(fs.readFileSync(path.join(__dirname,'exclusions.json'),'utf8'));
  const out=publication(data,rules);
  const json=JSON.stringify(out);
  const bytes=Buffer.byteLength(json)+['index.html','model.js','rights.html'].reduce((n,file)=>n+fs.statSync(path.join(__dirname,'web',file)).size,0);
  checkSize(out,bytes);
  const site=path.join(__dirname,'build/site'); fs.mkdirSync(site,{recursive:true});
  for(const file of ['index.html','model.js','rights.html']) fs.copyFileSync(path.join(__dirname,'web',file),path.join(site,file));
  fs.writeFileSync(path.join(site,'data.json'),json);
  console.log(`Staged ${out.assets.length} assets; ${data.assets.length-out.assets.length} excluded`);
}
module.exports={publication,checkSize};
