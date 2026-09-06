/* Pure browser data functions; also exercised by the Node regression suite. */
(function(root){
  "use strict";
  const fields = ["id","t","s","u","th","ty","st","l","lu","a","tg","tga","f","c","n","p","up","pc","seen","checked"];
  const changedFields = ["t","a","l","lu","u","c","n"];
  const http = value => { try { return ["http:","https:"].includes(new URL(value).protocol); } catch { return false; } };
  function asset(value){
    if (!value || typeof value !== "object" || Array.isArray(value) ||
        typeof value.t !== "string" || !value.t.trim() || typeof value.s !== "string" ||
        !http(value.u) || typeof value.l !== "string" || ![0,1].includes(value.c) || ![0,1].includes(value.n))
      throw new Error("Invalid asset record");
    const out = {};
    for (const key of fields) if (value[key] != null) {
      if (["c","n","p","up","pc","seen","checked"].includes(key)) {
        if (!Number.isFinite(value[key])) throw new Error("Invalid numeric field");
      } else if (typeof value[key] !== "string") throw new Error("Invalid text field");
      out[key] = value[key];
    }
    for (const key of ["th","lu"]) if (out[key] && !http(out[key])) throw new Error("Invalid link");
    return out;
  }
  function validateIndex(data){
    if (!data || !Array.isArray(data.assets) || !Array.isArray(data.sources) ||
        !data.stats || !Number.isFinite(data.generated)) throw new Error("Invalid index format");
    const ids = new Set();
    const assets = data.assets.map(value => {
      const a = asset(value), key = a.id || a.u;
      if (ids.has(key)) throw new Error("Duplicate asset identity");
      ids.add(key); return a;
    });
    for (const s of data.sources) {
      if (!s || typeof s.name !== "string" || (s.site && !http(s.site)) ||
          ![s.assets,s.commercial,s.dead].every(Number.isFinite)) throw new Error("Invalid source summary");
    }
    for (const key of ["total","commercial","nocredit","unclassifiable","dead","unchecked"])
      if (!Number.isFinite(data.stats[key])) throw new Error("Invalid index statistics");
    return {...data,assets};
  }
  function record(value){
    const a=asset(value);
    for(const key of ["credit_author","credit_notice","modifications","evidence_url"]){
      if(value[key]!=null && (typeof value[key]!=="string" || value[key].length>10000)) throw new Error("Invalid project note");
      a[key]=value[key]||"";
    }
    if(a.evidence_url && !http(a.evidence_url)) throw new Error("Evidence must be an HTTP(S) URL");
    for(const key of ["saved_at","reviewed_at"]){
      if(value[key]!=null && (!Number.isFinite(value[key]) || value[key]<0 || value[key]>8640000000000000)) throw new Error("Invalid save date");
      a[key]=value[key]||0;
    }
    return a;
  }
  function saved(value){
    const a=record(value);
    if(value.history!=null && !Array.isArray(value.history)) throw new Error("Invalid revision history");
    a.history=(value.history||[]).map(record);
    return a;
  }
  function readProject(text){
    const value = JSON.parse(text);
    const items = Array.isArray(value) ? value : value?.format === "magpie-project" && value.version === 1 ? value.assets : null;
    if (!Array.isArray(items) || items.length>10000) throw new Error("Invalid project backup");
    const result = items.map(saved), ids = new Set();
    for (const a of result) { const key=a.id||a.u; if(ids.has(key)) throw new Error("Duplicate saved asset"); ids.add(key); }
    return result;
  }
  function project(items){ return JSON.stringify({format:"magpie-project",version:1,assets:[...items].map(saved)},null,2); }
  function differences(old, current){
    if (!current) return ["not in current index"];
    return changedFields.filter(key => (old[key]||"") !== (current[key]||""));
  }
  function accept(old, current){
    const notes=record(old);
    // Only project notes survive outside history; omitted source fields are removals.
    for(const key of fields) delete notes[key];
    return {...asset(current),...notes,reviewed_at:0,saved_at:Date.now(),history:[...(old.history||[]),record(old)]};
  }
  function obligations(a){
    switch(a.l){
      case "cc0": return "CC0 dedication; no attribution requirement. Check the source for other rights.";
      case "cc_by": return "Attribution: credit the creator, link the licence, and indicate changes, and retain supplied notices. Do not add restrictions or technological measures that prevent exercising the licensed rights. Check the linked licence version.";
      case "cc_by_sa": return "Attribution and ShareAlike: credit, link the licence, indicate changes, and share adaptations under the required compatible licence. Retain supplied notices and do not add restrictions or technological measures that prevent exercising the licensed rights. Check the linked version.";
      case "oga_by": return "Attribution required. Follow the linked OGA-BY terms, including any attribution instructions on the source page.";
      case "gpl": return "Commercial use is permitted by GPL, with copyleft and source-distribution obligations where applicable. Review the exact licence and how the asset is used.";
      case "personal_only": return "Personal use only; commercial permission has not been established.";
      default: return "Terms are not established here. Review the source and its exact licence before use.";
    }
  }
  function previewAllowed(a){
    if(!a.th) return false;
    let url; try { url=new URL(a.th); } catch { return false; }
    if(url.protocol!=="https:") return false;
    if(a.s==="ambientcg") return url.hostname==="acg-media.struffelproductions.com";
    // API-supplied asset thumbnails only; website example/user renders are excluded.
    if(a.s==="polyhaven") return url.hostname==="cdn.polyhaven.com" && url.pathname.startsWith("/asset_img/thumbs/");
    if(a.s==="gameicons") return url.hostname==="cdn.jsdelivr.net" && url.pathname.startsWith("/gh/game-icons/icons@") &&
      (a.l==="cc0" || (a.l==="cc_by" && !!a.a && !!a.lu));
    return false;
  }
  function creditIssues(a){
    const issues=[];
    if(a.l!=="cc0" && !(a.credit_author||a.a)) issues.push("creator missing");
    if(!a.lu) issues.push("licence link missing");
    if(a.l==="oga_by" && a.lu==="https://opengameart.org/content/faq") issues.push("exact OGA-BY version not recorded; check the source");
    if(["unknown","store_eula","personal_only"].includes(a.l)) issues.push("usage terms need review");
    if(!a.reviewed_at) issues.push("source instructions not reviewed");
    if(a.l!=="cc0" && !a.modifications) issues.push("changes not recorded");
    return issues;
  }
  // Keep substring search; expand only common plural endings to whole words.
  function singular(word){
    if(/[^a-z]/.test(word) || word.length<4) return word;
    if(/ies$/.test(word) && word.length>4) return word.slice(0,-3)+"y";
    if(/(?:ches|shes|xes|sses|zzes)$/.test(word)) return word.slice(0,-2);
    if(/s$/.test(word) && !/(?:ss|us|is)$/.test(word)) return word.slice(0,-1);
    return word;
  }
  function matchesQuery(text,terms){
    let words;
    return terms.every(term=>{
      if(text.includes(term)) return true;
      const single=singular(term);
      if(single===term) return false;
      words ||= new Set(text.match(/[a-z]+/g)||[]);
      return words.has(single);
    });
  }
  function prepare(a){
    return {...a,_sourceHay:(a.t+" "+(a.tg||"")+" "+(a.a||"")+" "+a.s).toLowerCase(),
      _sourceTags:new Set((a.tg||"").split(",").filter(Boolean)),
      _h:(a.t+" "+(a.tg||"")+" "+(a.tga||"")+" "+(a.a||"")+" "+a.s).toLowerCase(),
      _t:new Set([...(a.tg||"").split(","),...(a.tga||"").split(",")].filter(Boolean))};
  }
  const api={matchesQuery,asset,validateIndex,readProject,project,differences,accept,obligations,prepare,previewAllowed,creditIssues,saved};
  if(typeof module!=="undefined") module.exports=api;
  else root.Magpie=api;
})(globalThis);
