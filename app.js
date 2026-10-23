import {loadEngine} from './engine.js';
const $=id=>document.getElementById(id), durations=[.25,.5,1,3,6,12,24,48,72],names=['15 minutes','30 minutes','1 hour','3 hours','6 hours','12 hours','1 day','2 days','3 days'],risks=['Low','Medium','High','Critical','Not assessed'];
const messages=['Capacity is sufficient for the simulated inflow. Continue routine maintenance.','Spare capacity is reduced. Inspect the drain and check for potential blockages.','Spare capacity is limited. Prioritize drain inspection and cleaning.','Inflow meets or exceeds effective capacity. This scenario indicates potential overflow.','There is no inflow and effective capacity is zero. The 0/0 ratio is undefined; inspect the conditions.'];
const fmt=(v,d=2)=>Number.isNaN(v)?'NA':!Number.isFinite(v)?'∞':v.toLocaleString('en-US',{minimumFractionDigits:d,maximumFractionDigits:d});
let engine, latest, rows;
function get(id){const el=$(id);if(el.value.trim()===''||!el.checkValidity())throw Error('Check the value for '+document.querySelector(`label[for="${id}"]`).textContent+'.');return Number(el.value);}
function result(c,i,a,cap,b){const qin=engine.dg_runoff(c,i,a),effective=engine.dg_effective(cap,b),ratio=engine.dg_ratio(qin,effective),risk=risks[engine.dg_risk(ratio)];return {qin,effective,ratio,risk};}
function render(){if(!engine)return;try{
 const manual=$('scenario').value==='manual';$('manualbox').hidden=!manual;
 const c=get('c'),area=get('area'),diameter=get('diameter'),n=get('roughness'),slope=get('slope'),b=get('blockage')/100;
 const intensity=manual?get('intensity'):engine.dg_intensity(+$('scenario').value),cap=engine.dg_capacity(diameter,n,slope);
 const r=result(c,intensity,area,cap,b);latest={...r,intensity,c,area,diameter,n,slope,cap,b};
 $('error').hidden=true;$('results').classList.remove('invalid');$('download').disabled=false;
 $('rainnote').textContent=`Intensity ${fmt(intensity)} mm/h · ${manual?'Manual input':'IDF · 10-year return period'}`;
 $('blockvalue').textContent=fmt(b*100,0)+'%';$('qin').textContent=fmt(r.qin);$('effective').textContent=fmt(r.effective);$('capnote').textContent=`of ${fmt(cap)} m³/s`;$('ratio').textContent=fmt(r.ratio,3);
 $('risk').textContent=r.risk;$('riskpanel').dataset.risk=r.risk;$('recommendation').textContent=messages[engine.dg_risk(r.ratio)];
 $('margin').textContent=fmt(Math.abs(r.effective-r.qin));$('marginlabel').textContent=r.qin>r.effective?'Excess inflow (m³/s)':'Remaining capacity (m³/s)';
 $('pipepercent').textContent=fmt(b*100,0)+'%';$('pipefill').style.height=b*100+'%';$('capacity').textContent=fmt(cap)+' m³/s';
 rows=durations.map((h,j)=>{const i=engine.dg_intensity(h);return {duration:h,name:names[j],intensity:i,...result(c,i,area,cap,b)}});
 $('comparison').innerHTML=rows.map(x=>`<tr class="${!manual&&x.duration===+$('scenario').value?'selected':''}"><td>${x.name}</td><td>${fmt(x.intensity)}</td><td>${fmt(x.qin)}</td><td>${fmt(x.ratio,3)}</td><td><span class="badge ${x.risk}">${x.risk}</span></td></tr>`).join('');
 draw(r.qin,cap,b,r.ratio);
 }catch(e){$('error').textContent=e instanceof WebAssembly.RuntimeError?'Value outside the calculation range. Check the parameters.':e.message;$('error').hidden=false;$('results').classList.add('invalid');$('download').disabled=true;}}
function draw(q,cap,b,ratio){const ymax=Math.min(10,Math.max(1.2,Number.isFinite(ratio)?ratio*1.2:2)),x=v=>48+v*5.3,y=v=>228-v/ymax*200;let svg='';for(const v of [0,.5,.8,1]){svg+=`<line x1="48" y1="${y(v)}" x2="578" y2="${y(v)}" stroke="#cbd9d1" stroke-dasharray="4 5"/><text x="36" y="${y(v)+4}" text-anchor="end" font-size="11" fill="#647774">${fmt(v,1)}</text>`;}[0,25,50,75,100].forEach(v=>svg+=`<text x="${x(v)}" y="251" text-anchor="middle" font-size="11" fill="#647774">${v}%</text>`);let path='',above=false;
 for(let z=0;z<100;z++){const v=engine.dg_ratio(q,engine.dg_effective(cap,z/100));if(!Number.isFinite(v)){path+='';continue;}if(v>ymax)above=true;path+=(path?' L':'M')+x(z)+' '+y(Math.min(v,ymax));}
 svg+=`<path d="${path}" fill="none" stroke="#008376" stroke-width="3"/><line x1="${x(b*100)}" y1="24" x2="${x(b*100)}" y2="228" stroke="#143e35" stroke-dasharray="3 4"/>`;
 if(Number.isFinite(ratio))svg+=`<circle cx="${x(b*100)}" cy="${y(Math.min(ratio,ymax))}" r="5" fill="#007d70" stroke="white" stroke-width="2"/>`;
 svg+='<text x="48" y="15" font-size="11" fill="#647774">Ratio R</text><text x="578" y="276" text-anchor="end" font-size="11" fill="#647774">Blockage</text>';
 $('chart').innerHTML=svg;$('chartnote').textContent=`R-axis capped at ${fmt(ymax,1)}${above?' · Curve clipped above this limit':''}. At 100%: ${q>0?'R = ∞ (Critical)':'R = NA (0/0)'}.`;
}
$('reset').onclick=()=>{for(const [id,v]of Object.entries({scenario:.25,blockage:0,c:.5334,area:60.7028,diameter:3.590,roughness:.011,slope:.01,intensity:0}))$(id).value=v;render();};
document.querySelectorAll('input,select').forEach(el=>el.addEventListener('input',render));document.querySelectorAll('[data-b]').forEach(el=>el.onclick=()=>{$('blockage').value=el.dataset.b;render();});
$('download').onclick=()=>{if(!latest)return;const v=latest;const header=['Scenario','C','Rainfall_i_mmh','Area_ha','Q_in_m3s','Original_Q_capacity','Blockage_Ratio','Effective_Q_capacity','Risk_Ratio_R','Risk_Level'];const num=n=>Number.isNaN(n)?'NA':!Number.isFinite(n)?'INF':n;const all=[{name:'Current scenario',...v},...rows];const csv=[header.join(','),...all.map(r=>[r.name,v.c,r.intensity,v.area,r.qin,v.cap,v.b,r.effective,num(r.ratio),r.risk].join(','))].join('\n');const url=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'}));const a=document.createElement('a');a.href=url;a.download='DrainGuard-results.csv';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
try{const response=await fetch('./engine.wasm');if(!response.ok)throw Error('Calculation engine unavailable');engine=await loadEngine(await response.arrayBuffer());$('engineStatus').textContent='● C++ engine connected';render();}catch(e){$('engineStatus').textContent='Engine not connected';$('error').textContent='The C++ engine could not load. Reload the page or check your connection.';$('error').hidden=false;}
