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
  function saved(value){
    const a = asset(value);
    if (value.saved_at != null && !Number.isFinite(value.saved_at)) throw new Error("Invalid save date");
    a.saved_at = value.saved_at || 0;
    a.history = (value.history || []).map(h => ({...asset(h),saved_at:Number.isFinite(h.saved_at)?h.saved_at:0}));
    if (a.history.length>100) throw new Error("Too many revisions");
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
    return {...asset(current),saved_at:Date.now(),history:[...(old.history||[]),{...asset(old),saved_at:old.saved_at||0}]};
  }
  function obligations(a){
    switch(a.l){
      case "cc0": return "CC0 dedication; no attribution requirement. Check the source for other rights.";
      case "cc_by": return "Attribution: credit the creator, link the licence, and indicate changes. Check the linked licence version.";
      case "cc_by_sa": return "Attribution and ShareAlike: credit, link the licence, indicate changes, and share adaptations under the required compatible licence. Check the linked version.";
      case "oga_by": return "Attribution required. Follow the linked OGA-BY terms, including any attribution instructions on the source page.";
      case "gpl": return "Commercial use is permitted by GPL, with copyleft and source-distribution obligations where applicable. Review the exact licence and how the asset is used.";
      case "personal_only": return "Personal use only; commercial permission has not been established.";
      default: return "Terms are not established here. Review the source and its exact licence before use.";
    }
  }
  function prepare(a){
    return {...a,_h:(a.t+" "+(a.tg||"")+" "+(a.tga||"")+" "+(a.a||"")+" "+a.s).toLowerCase(),
      _t:new Set([...(a.tg||"").split(","),...(a.tga||"").split(",")].filter(Boolean))};
  }
  const api={asset,validateIndex,readProject,project,differences,accept,obligations,prepare};
  if(typeof module!=="undefined") module.exports=api;
  else root.Magpie=api;
})(globalThis);
