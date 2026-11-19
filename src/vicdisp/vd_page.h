// Victron-Display: Weboberflaeche (eine Seite, direkt aus dem Flash)
#pragma once
#include <pgmspace.h>

static const char VD_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Victron-Display</title>
<style>
:root{--bg:#0F1317;--card:#181D22;--line:#272E36;--tx:#E8ECEF;--tx2:#9AA5B1;--ac:#3C8DFF;--ok:#5BD38A;--warn:#F3A43B;--err:#FF6A5C}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:760px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 2px}h2{font-size:16px;margin:0 0 10px}.sub{color:var(--tx2);margin:0 0 14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:0 0 14px}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 14px}.chip{border:1px solid var(--line);border-radius:999px;padding:3px 10px;font-size:13px;color:var(--tx2)}
.chip.ok{color:var(--ok);border-color:#2c5a3e}.chip.err{color:var(--err);border-color:#5a2c2c}
label.f{display:block;margin:0 0 10px}label.f span{display:block;font-size:13px;color:var(--tx2);margin:0 0 4px}
input,select{width:100%;background:#0F1317;color:var(--tx);border:1px solid var(--line);border-radius:10px;padding:9px 10px;font:inherit}
input[type=checkbox]{width:auto;margin:0 8px 0 0;transform:scale(1.2)}
.row{display:flex;gap:10px;flex-wrap:wrap}.row>*{flex:1;min-width:140px}
.ck{display:flex;align-items:center;margin:6px 0}
button{background:#232A31;color:var(--tx);border:1px solid var(--line);border-radius:10px;padding:8px 14px;font:inherit;cursor:pointer}
button.p{background:var(--ac);border-color:var(--ac);color:#fff}button.d{color:var(--err)}button:disabled{opacity:.5;cursor:default}
.dev{border-top:1px solid var(--line);padding:10px 0;display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.dev:first-child{border-top:0}.dev .t{flex:1;min-width:180px}.dev .v{color:var(--tx2);font-size:13px}
.hint{color:var(--tx2);font-size:13px;margin:6px 0 10px}.mono{font-family:ui-monospace,monospace}
.io{border:1px solid var(--line);border-radius:12px;padding:12px;margin:0 0 10px}.ioh{display:flex;gap:10px;align-items:center;margin:0 0 8px}.ioh b{flex:1}
.live{color:var(--tx2);font-size:13px}.sec{margin:16px 0 8px;font-size:15px}.pr{display:flex;gap:8px;align-items:flex-end;flex-wrap:wrap}.pr>*{flex:1;min-width:110px}
.pr .lbl{flex:none;width:70px;padding-bottom:18px;color:var(--tx2);font-size:13px}button.s{padding:5px 10px;font-size:13px}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#232A31;border:1px solid var(--line);border-radius:12px;padding:10px 16px;display:none;max-width:90%}
.dpp{margin:0 0 14px}.dph{display:flex;align-items:center;gap:8px;margin:0 0 6px}.dph b{flex:1}.dph select{width:auto;padding:4px 8px}
.scr{width:100%;max-width:320px;aspect-ratio:4/3;background:#000;border-radius:10px;padding:5px;display:grid;gap:5px}
.scr.n1{grid-template:1fr/1fr}.scr.n2{grid-template:1fr 1fr/1fr}.scr.n4{grid-template:1fr 1fr/1fr 1fr}
.tl{background:#1E252C;border:1px solid var(--line);border-radius:9px;padding:6px 9px;overflow:hidden;cursor:pointer;display:flex;flex-direction:column;justify-content:space-between;min-width:0;user-select:none;-webkit-user-select:none}
.tl .tn{font-size:12px;color:var(--tx2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.tl .tv{font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.scr.n1 .tv{font-size:30px}.scr.n2 .tv{font-size:21px}.scr.n1 .tn{font-size:14px}
.tl.sel{border-color:var(--ac);box-shadow:0 0 0 2px var(--ac)}.tl.em{background:transparent;border-style:dashed}.tl.ov{border-color:var(--ok)}
</style></head><body><main>
<h1 id="hName">Victron-Display</h1><p class="sub" id="hSub">…</p>
<div class="chips" id="chips"></div>

<div class="card" id="authCard" style="display:none"><h2 id="authT">Anmelden</h2><p class="hint" id="authH"></p>
<div id="pwSetup"><label class="f"><span>Standardpasswort (steht in der Anleitung)</span><input id="pOld" type="password" autocomplete="current-password"></label>
<label class="f"><span>Neues Passwort (mindestens 8 Zeichen)</span><input id="pNew" type="password" autocomplete="new-password"></label>
<label class="f"><span>Neues Passwort wiederholen</span><input id="pNew2" type="password" autocomplete="new-password"></label></div>
<div id="pwLogin"><label class="f"><span>Passwort</span><input id="pLogin" type="password" autocomplete="current-password"></label></div>
<div class="row"><button class="p" id="bAuth" style="flex:none">Weiter</button></div></div>
<div id="app" style="display:none">

<div class="card"><h2>Gerät und WLAN</h2>
<label class="f"><span>Name (Titelzeile des Displays)</span><input id="cName" maxlength="32"></label>
<div class="row"><label class="f"><span>Netzname (SSID) – Heimnetz oder WLAN des Cerbo GX</span><input id="cSsid" maxlength="32"></label>
<label class="f"><span>Passwort</span><input id="cWpass" type="password" maxlength="64" placeholder="unverändert"></label></div>
<div id="netList"></div>
<div class="row"><button id="bScanW" style="flex:none">Netze suchen</button><button class="p" id="bSaveW" style="flex:none">Speichern</button></div>
<p class="hint">Ohne WLAN-Verbindung öffnet das Display nach 30 s das Einrichtungs-WLAN „<span id="apName">SOC-Victron-…</span>“, die Einstellungsseite erscheint dann von selbst (sonst http://4.3.2.1/). <span id="mdns"></span></p>
<h2 style="margin-top:14px">Einrichtungs-WLAN</h2>
<p class="hint">Das eigene WLAN „<span class="apSsid">…</span>“ zum Einrichten (öffnet sich, wenn keine WLAN-Verbindung besteht). Sein WLAN-Passwort ist unabhängig vom Zugangspasswort dieser Seite.</p>
<label class="ck"><input type="checkbox" id="apOn">Mit WLAN-Passwort schützen (ab Werk: <b>panelsat</b>)</label>
<div class="row" id="apPwRow"><label class="f"><span>WLAN-Passwort (mindestens 8 Zeichen)</span><input id="apPw" type="password" maxlength="63" placeholder="unverändert" autocomplete="new-password"></label></div>
<p class="hint" id="apOpenHint" style="display:none">Ohne WLAN-Passwort kann sich jeder in der Nähe mit dem Einrichtungs-WLAN verbinden – Einstellungen ändern geht trotzdem nur mit dem Zugangspasswort.</p>
<div class="row"><button id="bApSave" style="flex:none">WLAN-Passwort speichern</button><button id="bPw" style="flex:none">Zugangspasswort ändern</button></div>
<p class="hint">Notfall: 3× kurz hintereinander aus- und einschalten (je unter 10 s) öffnet das Einrichtungs-WLAN sofort, 5× setzt WLAN-Verbindung, Zugangspasswort und WLAN-Passwort zurück (beide wieder <b>panelsat</b>). Geräte und Kacheln bleiben.</p></div>

<div class="card"><h2>Cerbo GX</h2>
<p class="hint">Nur nötig, wenn ein Cerbo GX (oder anderes GX-Gerät) im Fahrzeug ist. Am GX einschalten: Einstellungen → Dienste → MQTT im LAN (Klartext). Alles, was der GX liefert, erscheint dann automatisch als Kachel.</p>
<label class="ck"><input type="checkbox" id="cbOn">Cerbo GX verwenden</label>
<div id="cbBox">
<p class="hint" id="cbStatus"></p>
<label class="ck"><input type="checkbox" id="cbAuto">Automatisch suchen (172.24.24.1, Gateway, venus.local, einstein.local)</label>
<div class="row"><label class="f"><span>Adresse (Standard 172.24.24.1 = WLAN des Cerbo)</span><input id="cbHost" placeholder="172.24.24.1" class="mono"></label></div>
<div class="row"><label class="f"><span>Benutzer (nur bei MQTT mit Passwort)</span><input id="cbUser" autocomplete="off"></label>
<label class="f"><span>Passwort</span><input id="cbPass" type="password" placeholder="unverändert" autocomplete="off"></label></div>
</div>
<div class="row"><button class="p" id="bSaveC" style="flex:none">Speichern</button></div>
<div id="cbCtl"></div></div>

<div class="card"><h2>Truma LevelControl</h2>
<p class="hint">Gasflaschen-Füllstand per Bluetooth. „Sensoren suchen“, dann am Sensor kurz die Taste drücken (LED blinkt) und hinzufügen. Die Truma-App dabei schließen – der Sensor spricht nur mit einem Gerät.</p>
<div id="trList"></div>
<div class="row" style="margin-top:8px"><label class="f" style="flex:none;width:200px"><span>Messen alle … Minuten</span><input id="trInt" type="number" min="1" max="1440"></label></div>
<div class="row"><button id="bTrScan" style="flex:none">Sensoren suchen</button><button id="bTrMeasure" style="flex:none">Jetzt messen</button></div>
<div id="trFound"></div></div>

<div class="card"><h2>Mopeka</h2>
<p class="hint">Füllstand von Gasflaschen und Tanks per Bluetooth: Pro Check, Pro Plus, Pro Check Universal, Pro Check H2O, Pro-200, TD40/TD200, Lippert BottleCheck und Mopeka Check Standard/XL. „Sensoren suchen“, dann am Sensor die Sync-Taste drücken und hinzufügen. Die Sensoren senden von selbst – keine Kopplung, die Mopeka-App darf offen bleiben.</p>
<div id="mpList"></div>
<div class="row" style="margin-top:8px"><button id="bMpScan" style="flex:none">Sensoren suchen</button></div>
<div id="mpFound"></div></div>

<div id="tpList"></div>
<div class="row" style="margin-top:8px"><button id="bTpScan" style="flex:none">Sensoren suchen</button></div>
<div id="tpFound"></div></div>

<div class="card" id="bmCard" style="display:none"><h2>Akkus und Power Stations per Bluetooth</h2>
<p class="hint">Akkus mit eigenem Bluetooth-BMS: Super B (Epsilon), Power Quarry (VB…/CBT), Daly/Bulltron, JBD/Xiaoxiang, JK-BMS, LiTime (Redodo, PowerQueen), Renogy, Eco-Worthy, ANT und Vatrer – dazu Renogy-Laderegler, DC-DC-Lader, Wechselrichter und Smart Shunt. Power Stations (ungetestet): Bluetti, Allpowers, Anker SOLIX und EcoFlow – mit Schalter für den AC- und DC-Ausgang. Die App des Herstellers vorher schließen – das Gerät spricht nur mit einem. Bei Akkus liest das Display nur Werte und schaltet nichts.</p>
<p class="hint" id="bmSS" style="display:none"><b>Beta:</b> SuperSense-Tankmodule (Name „SSBT…“, bis 4 Tanksensoren, auch SuperSense PRO mit Bluetooth) werden hier ebenfalls gefunden – je Tanksensor eine Kachel mit Füllstand und Temperatur. Ungetestet: bitte Rückmeldung, ob die Werte stimmen. Die SuperSense-App dabei schließen.</p>
<div id="bmList"></div>
<div class="row" style="margin-top:8px"><button id="bBmScan" style="flex:none">Akkus suchen</button></div>
<div id="efBox" style="display:none;margin-top:10px"><h2 style="font-size:14px">EcoFlow-Konto</h2><p class="hint" id="efState"></p><div class="row"><label class="f"><span>E-Mail</span><input id="efMail" type="email" autocomplete="username"></label><label class="f"><span>Passwort (wird nicht gespeichert)</span><input id="efPw" type="password" autocomplete="current-password"></label></div><div class="row"><button id="bEfLogin" style="flex:none">Anmelden und Benutzer-ID holen</button></div><p class="hint">EcoFlow-Power-Stations verlangen die Benutzer-ID des Kontos. Das Display holt sie einmal über das Internet (WLAN mit Internet nötig).</p></div>
<div id="bmFound"></div>
<details style="margin-top:10px"><summary class="hint">Protokoll (für die Fehlersuche)</summary><pre id="bmLog" class="mono" style="white-space:pre-wrap;font-size:12px"></pre></details></div>

<div class="card"><h2>Victron per Bluetooth</h2>
<p class="hint">Auch ohne Cerbo GX: SmartSolar, SmartShunt/BMV, Orion, Ladegeräte direkt per Bluetooth („Instant Readout“). Pro Gerät den Schlüssel aus VictronConnect: Einstellungen → Produktinfo → Instant Readout → Details anzeigen.</p>
<div id="viList"></div>
<div class="row" style="margin-top:8px"><button id="bViScan" style="flex:none">Geräte suchen</button></div>
<div id="viFound"></div></div>

<div class="card"><h2>Ampel-LED (Rückseite)</h2>
<p class="hint">Die farbige LED auf der Rückseite zeigt einen Füllstand: bis 10 % rot, bis 50 % orange, darüber grün. Ohne Messwert bleibt sie aus.</p>
<label class="f"><span>LED zeigt</span><select id="ledMode"><option value="1">Automatisch (Batterie, sonst Gas)</option><option value="2">Batterie (Ladezustand)</option><option value="3">Gas (leerste Flasche)</option><option value="0">Aus</option></select></label>
<p class="hint" id="ledNow"></p>
<div class="row" style="margin-top:8px"><button id="bLedSave" style="flex:none">Speichern</button></div></div>

<div class="card" id="dpCard" style="display:none"><h2>Display</h2>
<p class="hint">Welche Werte das Display zeigt, in welcher Reihenfolge und unter welchem Namen. Neue Werte erscheinen automatisch hinten.</p>
<p class="hint">Vorschau des Displays. Kachel antippen, dann den Zielplatz antippen (belegt = tauschen, frei = verschieben) – am PC auch ziehen. Je Seite 1, 2 oder 4 Kacheln, weitere Seiten mit „+ Seite hinzufügen“. Erst „Speichern“ übernimmt es aufs Display.</p>
<div id="dpEd"></div><div id="dpSel"></div><div id="dpHid"></div>
<div class="row" style="margin-top:10px">
<label class="f"><span>Kacheln je Seite für neue Seiten</span><select id="dpPer"><option value="4">4 (2 × 2)</option><option value="2">2 (groß)</option><option value="1">1 (ganzer Bildschirm)</option></select></label>
<label class="f"><span>Helligkeit</span><input type="range" id="dpBright" min="20" max="255"></label>
<label class="f"><span>Dimmen nach</span><select id="dpDim"><option value="1">1 Minute</option><option value="2">2 Minuten</option><option value="5">5 Minuten</option><option value="10">10 Minuten</option><option value="0">nie</option></select></label>
<label class="ck"><input type="checkbox" id="dpAuto">Helligkeit automatisch (Lichtsensor vorne am Display)</label>
<label class="f"><span>Helligkeit bei Dunkelheit</span><input type="range" id="dpNight" min="5" max="255"></label>
<label class="f"><span>Dunkel ab Messwert</span><input type="number" id="dpThr" min="1" max="4095"></label>
<p class="hint"><span id="ldrNow">Lichtsensor: …</span> Größerer Wert = dunkler. Zum Einstellen: Raum abdunkeln, Wert ablesen und etwas darunter als Grenze eintragen.</p>
</div>
<div class="row" style="margin-top:8px"><button id="bDpSave" style="flex:none">Speichern</button><button id="bTcal" style="flex:none">Touch kalibrieren</button></div>
<p class="hint">Kalibrieren: auf dem Display nacheinander die vier Pfeilspitzen antippen. Geht auch ohne Browser: beim Einschalten 2 Sekunden auf den Bildschirm drücken.</p></div>

<div class="card"><h2>Firmware</h2>
<p class="hint" id="fwInfo"></p>
<p class="hint">Update: „Update-Modus starten“ – das Display startet in den kleinen Updater (gleiche Adresse, gleiches
Passwort). Dort die neue <b>firmware.bin</b> hochladen; danach startet die neue Firmware von selbst.</p>
<div class="row"><button id="bUpd" style="flex:none">Update-Modus starten</button><button id="bReboot" style="flex:none">Neu starten</button></div>
<details style="margin-top:8px"><summary class="hint">Updater selbst erneuern (updater.bin, max. 1 MB)</summary>
<div class="row"><input type="file" id="fwFile" accept=".bin"><button id="bFw" style="flex:none">Hochladen</button></div></details>
</div>
</div>
</main><div id="toast"></div>
<script>
const $=s=>document.querySelector(s);let S=null,loaded=false;
const esc=s=>String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function toast(t,err){const e=$('#toast');e.textContent=t;e.style.display='block';e.style.color=err?'var(--err)':'';clearTimeout(e._t);e._t=setTimeout(()=>e.style.display='none',3500)}
async function post(u,d){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||r.statusText);return j}
const act=(u,d,msg)=>post(u,d).then(j=>{if(msg)toast(msg);load();return j}).catch(e=>toast(e.message,true));
const sig=r=>r>-65?'stark':r>-80?'mittel':'schwach';
const num=(v,d,u)=>v==null?'':v.toFixed(d).replace('.',',')+' '+u;
const age=s=>s==null||s<0?'noch nichts':s<60?'gerade eben':s<3600?'vor '+Math.round(s/60)+' Min.':'vor '+Math.round(s/3600)+' Std.';
function fill(){
 $('#cName').value=S.name;$('#cSsid').value=S.wifi.ssid;$('#apName').textContent=S.id;$('#mdns').textContent=S.wifi.mdns?'Im Heimnetz auch unter '+S.wifi.mdns:'';
 $('#ledMode').value=S.led.mode;$('#cbOn').checked=S.cerbo.on;$('#cbBox').style.display=S.cerbo.on?'':'none';$('#trInt').value=S.truma.interval;$('#cbAuto').checked=S.cerbo.auto;$('#cbHost').value=S.cerbo.host;$('#cbUser').value=S.cerbo.user;$('#cbPass').placeholder=S.cerbo.has_pass?'unverändert':'kein Passwort';
}
function render(){
 const nm=S.name||S.id;$('#hName').textContent=nm;document.title=nm;
 $('#hSub').textContent='Kennung '+S.id+(S.wifi.ip?' · '+S.wifi.ip:'');
 const c=[];c.push(S.wifi.connected?['ok','WLAN '+S.wifi.ssid+' ('+S.wifi.rssi+' dBm)']:['err',S.wifi.ssid?'WLAN getrennt':'kein WLAN eingerichtet']);
 if(S.wifi.ap)c.push(['',S.wifi.force_ap?'Einrichtungs-WLAN an (Mehrfach-Reset, bis zum Neustart)':'Einrichtungs-WLAN an']);
 if(S.cerbo.on)c.push([S.cerbo.connected?'ok':'err','Cerbo: '+(S.cerbo.connected?'verbunden'+(S.cerbo.portal?' ('+S.cerbo.portal+')':''):(S.cerbo.status||'nicht verbunden'))]);
 if(S.victron.devices.length)c.push(['ok','Victron: '+S.victron.devices.length+' Gerät(e)']);
 if(S.truma.sensors.length)c.push(['ok','Gas: '+S.truma.sensors.length+' Sensor(en)']);
 if(S.mopeka&&S.mopeka.sensors.length)c.push(['ok','Mopeka: '+S.mopeka.sensors.length+' Sensor(en)']);
 $('#chips').innerHTML=c.map(x=>`<span class="chip ${x[0]}">${esc(x[1])}</span>`).join('');
 $('#cbStatus').textContent=(S.cerbo.connected?'Verbunden mit '+S.cerbo.host:S.cerbo.host?'Adresse '+S.cerbo.host+' – '+(S.cerbo.status||'nicht verbunden'):'Noch keine Adresse.')+(S.cerbo.find?' · '+S.cerbo.find:'');
 $('#fwInfo').textContent='Version '+S.fw+', freier Speicher '+Math.round(S.heap/1024)+' KB, läuft seit '+Math.round(S.uptime/60)+' Min.'+(S.upd_slot?(S.upd_ok?' · Updater vorhanden':' · Updater fehlt (einmal per USB aufspielen)'):' · alte Speicheraufteilung (einmal per USB neu aufspielen)');$('#bUpd').disabled=!S.upd_ok;
 if(document.activeElement&&document.activeElement.closest('#viList,#trList,#bmList,#mpList'))return;
 const T=S.truma;
 $('#trList').innerHTML=T.sensors.length?T.sensors.map(x=>`<div class="dev"><div class="t"><input value="${esc(x.name)}" data-trn="${x.mac}" maxlength="32">
  <div class="v">${x.has?x.percent+' % · '+num(x.kg,1,'kg')+' · Akku '+x.battery+' % · '+age(x.age):esc(x.status)}${x.busy?' · misst …':''} · <span class="mono">${x.mac}</span></div></div>
  <button data-trm="${x.mac}">Messen</button><button class="d" data-trr="${x.mac}">Entfernen</button></div>`).join(''):'<p class="hint">Noch kein Sensor eingerichtet.</p>';
 $('#bTrScan').disabled=T.scanning||T.sensors.length>=T.max;$('#bTrScan').textContent=T.scanning?'Suche läuft … (Taste am Sensor drücken)':'Sensoren suchen';
 $('#trFound').innerHTML=T.scanning||T.found.length?'<h2 style="margin-top:12px">Gefunden</h2>'+(T.found.length?T.found.map((f,i)=>`<div class="dev"><div class="t mono">${f.mac}<div class="v">Signal ${sig(f.rssi)}${f.configured?', eingerichtet':''}</div></div><button data-tra="${i}" ${f.configured?'disabled':''}>Hinzufügen</button></div>`).join(''):'<p class="hint">Noch nichts gefunden …</p>'):'';
 const M=S.mopeka;if(M){
  const tk=(x)=>`<select data-mpt="${x.mac}">${M.tanks.map((n,i)=>`<option value="${i}" ${x.tank==i?'selected':''}>${esc(n)}</option>`).join('')}<option value="255" ${x.tank==255?'selected':''}>Eigene Maße</option></select>`;
  $('#mpList').innerHTML=M.sensors.length?M.sensors.map(x=>`<div class="dev"><div class="t"><input value="${esc(x.name)}" data-mpn="${x.mac}" maxlength="32">
   <div class="v">${esc(x.model)} · ${x.has?[x.percent+' %',x.amount>0?num(x.amount,x.medium==0?1:0,x.unit):'',x.mm+' mm','Akku '+x.battery+' %',x.temp+' °C','Signal '+['schlecht','mäßig','gut','sehr gut'][x.quality||0],age(x.age)].filter(Boolean).join(' · '):esc(x.status)} · <span class="mono">${x.mac}</span></div>
   <div class="v">Flasche/Tank: ${tk(x)}${x.tank==255?` leer <input data-mpe="${x.mac}" type="number" min="0" max="3000" value="${x.empty}" style="width:5em;padding:4px 6px"> mm · voll <input data-mpf="${x.mac}" type="number" min="1" max="3000" value="${x.full}" style="width:5em;padding:4px 6px"> mm · Inhalt <input data-mpc="${x.mac}" type="number" min="0" step="0.1" value="${x.cap}" style="width:5em;padding:4px 6px"> ${esc(x.unit)}`:''}</div></div>
   <button class="d" data-mpr="${x.mac}">Entfernen</button></div>`).join(''):'<p class="hint">Noch kein Sensor eingerichtet.</p>';
  $('#bMpScan').disabled=M.scanning||M.sensors.length>=M.max;$('#bMpScan').textContent=M.scanning?'Suche läuft … (Sync-Taste am Sensor drücken)':'Sensoren suchen';
  $('#mpFound').innerHTML=M.scanning||M.found.length?'<h2 style="margin-top:12px">Gefunden</h2>'+(M.found.length?M.found.map((f,i)=>`<div class="dev"><div class="t">${esc(f.model||'Mopeka')} <span class="mono">${f.mac}</span><div class="v">Signal ${sig(f.rssi)}${f.button?', Sync-Taste gedrückt':''}${f.configured?', eingerichtet':''}</div></div><button data-mpa="${i}" ${f.configured?'disabled':''}>Hinzufügen</button></div>`).join(''):'<p class="hint">Noch nichts gefunden …</p>'):''}
 const B=S.bms;if(B){$('#bmCard').style.display='';$('#bmSS').style.display=B.supersense?'':'none';
  $('#bmList').innerHTML=B.devices.length?B.devices.map(x=>`<div class="dev"><div class="t"><input value="${esc(x.name)}" data-bmn="${x.mac}" maxlength="32">
   <div class="v">${esc(x.model)} · ${x.has&&x.dev?esc(x.sum)+' · '+(x.connected?age(x.age):'getrennt'):x.has?[x.soc+' %',x.v!=null?num(x.v,2,'V'):'',x.a!=null?num(x.a,1,'A'):'',x.soh!=null?'SoH '+x.soh+' %':'',x.chg?'lädt':x.dis?'entlädt':'',x.low?'Ladung niedrig':'',x.ok?'':'BMS meldet Alarm',x.t!=null?num(x.t,0,'°C'):'',x.cyc!=null?x.cyc+' Zyklen':'',x.cells?'Zellen '+x.cells.map(c=>(c/1000).toFixed(3)).join(' / ')+' V':'',x.ttg>0?'leer in '+Math.floor(x.ttg/3600)+' h '+Math.floor(x.ttg%3600/60)+' min':'',x.connected?age(x.age):'getrennt'].filter(Boolean).join(' · '):esc(x.status)} · <span class="mono">${x.mac}</span></div>${x.pin!=null?`<div class="v">PIN / Passwort: <input data-bmp="${x.mac}" maxlength="32" style="width:7em;padding:4px 8px" placeholder="${x.pin?'gespeichert':'keine'}">${x.pin_need&&!x.pin?' <b style="color:var(--err)">– Akku verlangt eine PIN</b>':''}</div>`:''}</div>
   ${x.ac!=null?`<button data-bmo="${x.mac}" data-ac="1" data-on="${x.ac==1?0:1}">AC ${x.ac==1?'aus':'ein'}</button>`:''}${x.dc!=null?`<button data-bmo="${x.mac}" data-ac="0" data-on="${x.dc==1?0:1}">DC ${x.dc==1?'aus':'ein'}</button>`:''}<button class="d" data-bmr="${x.mac}">Entfernen</button></div>`).join(''):'<p class="hint">Noch kein Akku eingerichtet.</p>';
  $('#bBmScan').disabled=B.scanning||B.devices.length>=B.max;$('#bBmScan').textContent=B.scanning?'Suche läuft …':'Akkus suchen';
  $('#bmFound').innerHTML=B.scanning||B.found.length?'<h2 style="margin-top:12px">Gefunden</h2>'+(B.found.length?B.found.map((f,i)=>`<div class="dev"><div class="t">${esc(f.name||'Akku')} <span class="mono">${f.mac}</span><div class="v">${({1:'Super B',2:'Power Quarry / Shentec',3:'Power Quarry / Shentec',4:'Daly/Bulltron',5:'JBD',6:'JK-BMS',7:'LiTime',8:'Renogy',9:'Eco-Worthy',10:'ANT',11:'Vatrer',12:'Renogy BT-1/BT-2',13:'Renogy Laderegler',14:'Renogy DC-DC-Lader',15:'Renogy Wechselrichter',16:'Renogy Smart Shunt',17:'Renogy Pro',18:'Bluetti',19:'Allpowers',20:'Anker SOLIX',21:'Anker SOLIX C300',22:'Anker SOLIX Gen 2',23:'Anker SOLIX F3800',24:'EcoFlow',25:'EcoFlow',26:'SuperSense Tankmodul (Beta)'})[f.model]||'Power Quarry'+(f.product?' '+esc(f.product):'')} · Signal ${sig(f.rssi)}${f.service?', Super-B-Dienst':''}${f.configured?', eingerichtet':''}</div></div><button data-bma="${i}" ${f.configured?'disabled':''}>Hinzufügen</button></div>`).join(''):'<p class="hint">Noch nichts gefunden … Akku eingeschaltet und in der Nähe?</p>'):'';
  $('#efBox').style.display=B.ecoflow?'':'none';if(B.ecoflow)$('#efState').textContent=(B.ef_uid_set?'Benutzer-ID gespeichert ('+(B.ef_uid_hint||'')+').':'Noch keine Benutzer-ID.')+(B.ef_login_text?' '+B.ef_login_text:'');
  $('#bmLog').textContent=(B.log||[]).join('\n')||'–'}
 const V=S.victron;
 $('#viList').innerHTML=V.devices.length?V.devices.map(x=>`<div class="dev"><div class="t"><input value="${esc(x.name)}" data-vin="${x.mac}" maxlength="32">
  <div class="v">${esc(x.kind_text)} · ${x.has?[x.pv!=null?num(x.pv,0,'W'):'',x.soc!=null?num(x.soc,0,'%'):'',num(x.v,2,'V'),num(x.i,1,'A'),x.state_text||''].filter(Boolean).join(' · '):esc(x.status)} · <span class="mono">${x.mac}</span></div></div>
  <button data-vik="${x.mac}">Schlüssel</button><button class="d" data-vir="${x.mac}">Entfernen</button></div>`).join(''):'<p class="hint">Noch kein Gerät eingerichtet.</p>';
 $('#bViScan').disabled=V.scanning||V.devices.length>=V.max;$('#bViScan').textContent=V.scanning?'Suche läuft …':'Geräte suchen';
 $('#viFound').innerHTML=V.scanning||V.found.length?'<h2 style="margin-top:12px">Gefunden</h2>'+(V.found.length?V.found.map((f,i)=>`<div class="dev"><div class="t">${esc(f.name||'Victron')} <span class="mono">${f.mac}</span><div class="v">Signal ${sig(f.rssi)}${f.configured?', eingerichtet':''}</div></div><button data-via="${i}" ${f.configured?'disabled':''}>Hinzufügen</button></div>`).join(''):'<p class="hint">Noch nichts gefunden …</p>'):'';
}
const MODES=[[[3,'Ein'],[4,'Aus'],[1,'Nur Laden'],[2,'Nur Wechselrichter']],[[1,'Ein'],[4,'Aus']]];
function renderCtl(){if(document.activeElement&&document.activeElement.closest('#cbCtl'))return;
 const C=S.cerbo.on?(S.cerbo.ctl||[]):[];$('#cbCtl').innerHTML=C.map(x=>`<h2 style="margin-top:14px">${esc(x.name)}</h2>
 ${x.limit!=null?`<div class="pr"><label class="f"><span>Netzstrom-Begrenzung (A)</span><input type="number" min="0" max="100" step="0.5" value="${x.limit}" data-cl="${x.key}"></label><button data-cls="${x.key}" style="flex:none;margin-bottom:10px">Setzen</button></div>`:''}
 ${x.mode!=null?`<div class="row">${MODES[x.set].map(m=>`<button data-cm="${x.key}" data-mv="${m[0]}" ${m[0]===x.mode?'class="p"':''} style="flex:none">${m[1]}</button>`).join('')}</div>`:''}`).join('')}
// Akku hinzufuegen: Name, "Ist eine Kopplungs-PIN noetig?" Ja/Nein, bei Ja die PIN (wird beim Koppeln eingegeben)
function bmAddDlg(f){const o=document.createElement('div');o.style.cssText='position:fixed;inset:0;background:rgba(0,0,0,.6);display:flex;align-items:center;justify-content:center;padding:16px;z-index:9';
 o.innerHTML=`<div class="card" style="width:100%;max-width:380px;margin:0"><h2>Akku hinzufügen</h2><p class="hint mono">${esc(f.name||'')} ${f.mac}</p>
 <label class="f"><span>Name, z. B. „Bordbatterie“</span><input id="dN" maxlength="32"></label>
 <label class="f"><span>Ist eine Kopplungs-PIN nötig?</span><select id="dQ"><option value="0">Nein</option><option value="1">Ja</option></select></label>
 <label class="f" id="dPl" style="display:none"><span>Kopplungs-PIN bzw. Passwort aus der Akku-App (steht auf dem Akku, in der Anleitung bzw. in der App)</span><input id="dP" maxlength="32" autocomplete="off"></label>
 <div style="display:flex;gap:8px;justify-content:flex-end"><button id="dC">Abbrechen</button><button class="p" id="dO">Hinzufügen</button></div></div>`;
 document.body.appendChild(o);const q=s=>o.querySelector(s);q('#dN').value=f.name||'Akku';q('#dQ').value=f.model==1?'1':'0';if(f.model==27||f.model==28)q('#dQ').parentNode.style.display='none';
 const sh=()=>{q('#dPl').style.display=q('#dQ').value=='1'?'':'none'};sh();q('#dQ').onchange=()=>{sh();if(q('#dQ').value=='1')q('#dP').focus()};
 q('#dC').onclick=()=>o.remove();
 q('#dO').onclick=()=>{const pin=q('#dQ').value=='1'?q('#dP').value.replace(/[^0-9A-Za-z]/g,''):'';if(q('#dQ').value=='1'&&!pin)return toast('Bitte die PIN bzw. das Passwort eingeben.',true);
  o.remove();act('/api/bms',{add:{mac:f.mac,addr_type:f.addr_type,name:q('#dN').value.trim()||'Akku',model:f.model,cap:f.cap,cells:f.cells,pin:pin}},pin?'Eingerichtet – koppele mit PIN …':'Eingerichtet – verbinde …')}}
document.addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;const d=b.dataset;
 if(d.cls){const i=document.querySelector(`[data-cl="${d.cls}"]`);act('/api/cerbo',{key:d.cls,limit:+i.value},'Begrenzung gesendet.')}
 if(d.cm)act('/api/cerbo',{key:d.cm,mode:+d.mv},'Schalter gesendet.')});
let AUTH=null;
function showAuth(mode){AUTH=mode;$('#app').style.display='none';$('#authCard').style.display='';
 const setup=mode==='setup';$('#pwSetup').style.display=setup?'':'none';$('#pwLogin').style.display=setup?'none':'';
 $('#authT').textContent=setup?'Erste Einrichtung: Passwort vergeben':'Anmelden';
 $('#authH').textContent=setup?'Zum Schutz zuerst das Standardpasswort eingeben und ein eigenes Zugangspasswort für diese Seite vergeben. Das WLAN-Passwort des Einrichtungs-WLANs stellst du danach getrennt ein.':'Passwort des Displays eingeben.';
 $('#hSub').textContent=''}
$('#bAuth').onclick=async()=>{try{
 if(AUTH==='setup'){if($('#pNew').value!==$('#pNew2').value)return toast('Die neuen Passwörter sind nicht gleich.',true);
  await post('/api/setpw',{old:$('#pOld').value,new:$('#pNew').value});toast('Zugangspasswort gespeichert.')}
 else await post('/api/login',{pw:$('#pLogin').value});
 $('#authCard').style.display='none';loaded=false;load();dpLoad()}catch(e){toast(e.message,true)}};
$('#bPw').onclick=async()=>{const o=prompt('Aktuelles Zugangspasswort');if(o==null)return;const n=prompt('Neues Passwort (mindestens 8 Zeichen)');if(!n)return;if(prompt('Neues Passwort wiederholen')!==n)return toast('Nicht gleich.',true);
 try{await post('/api/setpw',{old:o,new:n});toast('Passwort geändert.')}catch(e){toast(e.message,true)}};
async function apLoad(){try{const r=await fetch('/api/appw');if(!r.ok)return;const j=await r.json();$('#apOn').checked=j.on;apShow();document.querySelectorAll('.apSsid').forEach(e=>e.textContent=j.ssid||'…');$('#apPw').placeholder=j.custom?'unverändert':'unverändert (panelsat)'}catch(e){}}
function apShow(){$('#apPwRow').style.display=$('#apOn').checked?'':'none';$('#apOpenHint').style.display=$('#apOn').checked?'none':''}
$('#apOn').onchange=apShow;
$('#bApSave').onclick=()=>{const pw=$('#apPw').value;if($('#apOn').checked&&pw&&pw.length<8)return toast('Das WLAN-Passwort braucht mindestens 8 Zeichen.',true);
 post('/api/appw',{on:$('#apOn').checked,pw:pw}).then(()=>{$('#apPw').value='';toast('Gespeichert – das Einrichtungs-WLAN startet gleich mit der neuen Einstellung neu.');apLoad()}).catch(e=>toast(e.message,true))};
async function load(){try{const r=await fetch('/api/state');if(r.status===401){const j=await r.json();if(AUTH!==j.auth)showAuth(j.auth);return}
 AUTH=null;$('#authCard').style.display='none';$('#app').style.display='';S=await r.json();if(!loaded){fill();apLoad();loaded=true}render();renderCtl();ledShow();ldrShow()}catch(e){$('#hSub').textContent='Keine Verbindung zum Display …'}}
$('#bScanW').onclick=async()=>{$('#bScanW').disabled=true;$('#bScanW').textContent='Suche …';try{const r=await fetch('/api/wifiscan');const j=await r.json();if(!r.ok)throw new Error(j.error||'Suche fehlgeschlagen');
 $('#netList').innerHTML=j.networks.length?j.networks.map((n,i)=>`<div class="dev"><div class="t">${esc(n.ssid)}<div class="v">Signal ${sig(n.rssi)}${n.open?' · offen':''}</div></div><button data-net="${i}" style="flex:none">Wählen</button></div>`).join(''):'<p class="hint">Keine Netze gefunden.</p>';
 $('#netList')._n=j.networks}catch(e){toast(e.message,true)}$('#bScanW').disabled=false;$('#bScanW').textContent='Netze suchen'};
$('#netList').addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;const n=$('#netList')._n[+b.dataset.net];$('#cSsid').value=n.ssid;$('#netList').innerHTML='';$('#cWpass').value='';$('#cWpass').focus();toast('„'+n.ssid+'“ gewählt – Passwort eingeben und speichern.')});
$('#bSaveW').onclick=()=>post('/api/config',{name:$('#cName').value,wifi:{ssid:$('#cSsid').value,pass:$('#cWpass').value}}).then(j=>{toast(j.restart?'Gespeichert – startet neu und verbindet sich …':'Gespeichert.');if(!j.restart)load()}).catch(e=>toast(e.message,true));
$('#bSaveC').onclick=()=>act('/api/config',{cerbo:{on:$('#cbOn').checked,auto:$('#cbAuto').checked,host:$('#cbHost').value.trim(),user:$('#cbUser').value,pass:$('#cbPass').value}},'Gespeichert – verbindet sich …');
function ledShow(){const l=S.led;$('#ledNow').textContent=l.mode==0?'':l.pct<0?'Zurzeit kein Wert – LED aus.':'Zurzeit: '+l.src+' '+l.pct+' % – '+(l.pct<=10?'rot':l.pct<=50?'orange':'grün')+'.'}
$('#bLedSave').onclick=()=>act('/api/config',{led:+$('#ledMode').value},'Gespeichert.');
$('#cbOn').onchange=()=>{$('#cbBox').style.display=$('#cbOn').checked?'':'none'};
$('#bTcal').onclick=()=>act('/api/display',{calibrate:true},'Jetzt auf dem Display die Pfeilspitzen antippen.');
$('#trInt').onchange=e=>act('/api/truma',{interval:+e.target.value},'Gespeichert.');
$('#bTrScan').onclick=()=>act('/api/truma',{scan:true},'Suche gestartet – jetzt am Sensor die Taste drücken.');
$('#bTrMeasure').onclick=()=>act('/api/truma',{measure:''},'Messung gestartet.');
$('#bViScan').onclick=()=>act('/api/victron',{scan:true},'Suche gestartet (3 Minuten).');
$('#bMpScan').onclick=()=>act('/api/mopeka',{scan:true},'Suche gestartet – jetzt am Sensor die Sync-Taste drücken.');
const mpTank=mac=>{const x=S.mopeka.sensors.find(s=>s.mac==mac);if(!x)return;const v=a=>{const e=document.querySelector(`[data-${a}="${mac}"]`);return e?+e.value:null};
 const t=+document.querySelector(`[data-mpt="${mac}"]`).value;act('/api/mopeka',{tank:{mac,tank:t,empty:v('mpe')??x.empty,full:v('mpf')??x.full,cap:v('mpc')??x.cap}},'Gespeichert.')};
$('#bBmScan').onclick=()=>act('/api/bms',{scan:true},'Suche gestartet (1 Minute).');
$('#bEfLogin').onclick=()=>{const m=$('#efMail').value.trim(),w=$('#efPw').value;if(!m||!w)return toast('E-Mail und Passwort eingeben.',true);act('/api/bms',{ef_login:{email:m,password:w}},'Anmeldung läuft …');$('#efPw').value=''};
document.addEventListener('change',e=>{const t=e.target,d=t.dataset;
 if(d.vin)act('/api/victron',{rename:{mac:d.vin,name:t.value}},'Name gespeichert.');
 if(d.trn)act('/api/truma',{rename:{mac:d.trn,name:t.value}},'Name gespeichert.');
 if(d.mpn)act('/api/mopeka',{rename:{mac:d.mpn,name:t.value}},'Name gespeichert.');
 if(d.mpt||d.mpe||d.mpf||d.mpc)mpTank(d.mpt||d.mpe||d.mpf||d.mpc);
 if(d.bmn)act('/api/bms',{rename:{mac:d.bmn,name:t.value}},'Name gespeichert.');
 if(d.bmp)act('/api/bms',{pin:{mac:d.bmp,pin:t.value}},'PIN gespeichert – verbinde neu.');
});
document.addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;const d=b.dataset;
 if(d.vir&&confirm('Gerät entfernen?'))act('/api/victron',{remove:d.vir});
 if(d.trm)act('/api/truma',{measure:d.trm},'Messung gestartet.');
 if(d.trr&&confirm('Sensor entfernen?'))act('/api/truma',{remove:d.trr});
 if(d.mpr&&confirm('Sensor entfernen?'))act('/api/mopeka',{remove:d.mpr});
 if(d.mpa){const f=S.mopeka.found[+d.mpa];const n=prompt('Name, z. B. „Flasche links“','Gasflasche '+(S.mopeka.sensors.length+1));if(n!=null)act('/api/mopeka',{add:{mac:f.mac,name:n,tank:0}},'Eingerichtet – Flasche/Tank unten auswählen.')}
 if(d.bmr&&confirm('Akku entfernen?'))act('/api/bms',{remove:d.bmr});
 if(d.bmo)act('/api/bms',{output:{mac:d.bmo,ac:d.ac=='1',on:d.on=='1'}},'Gesendet.');
 if(d.bma)bmAddDlg(S.bms.found[+d.bma]);
 if(d.tra){const f=S.truma.found[+d.tra];const n=prompt('Name, z. B. „Flasche links“','Gasflasche '+(S.truma.sensors.length+1));if(n!=null)act('/api/truma',{add:{mac:f.mac,addr_type:f.addr_type,name:n}},'Eingerichtet.')}
 if(d.vik){const k=prompt('Schlüssel (32 Zeichen)');if(k)act('/api/victron',{key:{mac:d.vik,key:k.replace(/[\s:-]/g,'')}},'Schlüssel gespeichert.')}
 if(d.via){const f=S.victron.found[+d.via];const k=prompt('Schlüssel (32 Zeichen) für '+(f.name||f.mac));if(k){const n=prompt('Name',f.name||'Victron');act('/api/victron',{add:{mac:f.mac,key:k.replace(/[\s:-]/g,''),name:n||''}},'Eingerichtet.')}}
});
$('#bUpd').onclick=async()=>{if(!confirm('In den Update-Modus starten? Die Anzeige ist bis zum Update (oder 15 Minuten) aus.'))return;
 try{await post('/api/updater',{});toast('Starte den Updater … die Seite lädt gleich neu.');setTimeout(()=>location.reload(),12000)}catch(e){toast(e.message,true)}};
$('#bFw').onclick=async()=>{const f=$('#fwFile').files[0];if(!f)return toast('Erst eine .bin-Datei wählen',true);if(S.upd_slot&&f.size>S.upd_slot)return toast('Zu groß für den Updater-Bereich – die Firmware bitte über den Update-Modus aufspielen.',true);const fd=new FormData();fd.append('fw',f);$('#bFw').disabled=true;$('#bFw').textContent='Lädt hoch …';
 try{const r=await fetch('/api/update?size='+f.size,{method:'POST',body:fd});const j=await r.json();if(j.ok){toast('Firmware übernommen – Neustart …');setTimeout(()=>location.reload(),9000)}else toast(j.error||'Fehlgeschlagen',true)}catch(e){toast('Hochladen fehlgeschlagen',true)}$('#bFw').disabled=false;$('#bFw').textContent='Hochladen'};
$('#bReboot').onclick=()=>confirm('Display neu starten?')&&act('/api/reboot',{},'Startet neu …');
let DP=null,dSel=null,dG=null;
function ldrShow(){const l=S.light;if(!l||l.ldr<0)return;$('#ldrNow').textContent='Lichtsensor zurzeit: '+l.ldr+' – '+(l.night?'dunkel':'hell')+(l.auto?'.':' (Automatik aus).')}
const dPl=p=>p<DP.pl.length?DP.pl[p]:DP.per,dT=k=>DP.tiles.find(t=>t.k==k);
// Seiten aufbauen wie das Display: feste Seite aus den Einstellungen, der Rest fliesst hinten auf neue Seiten
function dpBuild(){const P=[],au=[];DP.tiles.filter(t=>t.on).forEach(t=>{if(t.pg!=null&&t.pg<16){while(P.length<=t.pg)P.push({n:dPl(P.length),k:[]});P[t.pg].k.push(t.k)}else au.push(t.k)});
 P.forEach(p=>au.push(...p.k.splice(p.n)));for(let i=0;i<au.length;){const n=dPl(P.length);P.push({n,k:au.slice(i,i+n)});i+=n}
 if(!P.length)P.push({n:dPl(0),k:[]});DP.P=P}
async function dpLoad(){try{const r=await fetch('/api/display');if(!r.ok)return;DP=await r.json();DP.pl=DP.pl||[];dSel=null;dpBuild();dpRender()}catch(e){}}
// zu viele Kacheln auf einer Seite: Ueberzaehlige an den Anfang der naechsten Seite
function dpFix(){const P=DP.P;for(let p=0;p<P.length;p++){const x=P[p].k.splice(P[p].n);if(x.length){if(!P[p+1])P.push({n:DP.per,k:[]});P[p+1].k.unshift(...x)}}}
// Kachel von Platz a nach Platz b: belegt = tauschen, frei = verschieben
function dpMove(a,b){const P=DP.P,ka=P[a.p].k[a.i];if(ka==null||(a.p==b.p&&a.i==b.i))return;const kb=P[b.p].k[b.i];
 if(kb!=null){P[a.p].k[a.i]=kb;P[b.p].k[b.i]=ka}else{P[a.p].k.splice(a.i,1);P[b.p].k.push(ka)}}
const dFlat=()=>{const f=[];DP.P.forEach((p,pi)=>p.k.forEach((k,i)=>f.push({p:pi,i})));return f};
function dpRender(){if(!DP)return;$('#dpCard').style.display='';let h='';
 DP.P.forEach((pg,p)=>{let c='';for(let i=0;i<pg.n;i++){const k=pg.k[i],t=k!=null&&dT(k),sel=dSel&&dSel.p==p&&dSel.i==i;
   c+=t?`<div class="tl${sel?' sel':''}" draggable="true" data-p="${p}" data-i="${i}"><div class="tn">${esc(t.n||t.name)}</div><div class="tv">${esc((t.value||'–').split('  ·  ')[0])}</div></div>`:`<div class="tl em" data-p="${p}" data-i="${i}"></div>`}
  h+=`<div class="dpp"><div class="dph"><b>Seite ${p+1}</b>${!pg.k.length&&DP.P.length>1?`<button class="s d" data-dpdel="${p}">Seite löschen</button>`:''}<select data-pl="${p}">${[4,2,1].map(v=>`<option value="${v}"${v==pg.n?' selected':''}>${v} ${v==1?'Kachel':'Kacheln'}</option>`).join('')}</select></div><div class="scr n${pg.n}">${c}</div></div>`});
 $('#dpEd').innerHTML=(DP.tiles.length?h:'<p class="hint">Noch keine Werte – erst Geräte oder Anschlüsse einrichten.</p>')+(DP.tiles.length?'<button id="dpAdd" style="margin:0 0 12px">+ Seite hinzufügen</button>':'');
 const sk=dSel&&DP.P[dSel.p].k[dSel.i],t=sk!=null&&sk!==undefined&&dT(sk),F=dFlat(),fi=dSel?F.findIndex(x=>x.p==dSel.p&&x.i==dSel.i):-1;
 $('#dpSel').innerHTML=t?`<div class="io"><div class="ioh"><b>${esc(t.name)}</b><button class="s" data-dmv="-1" ${fi>0?'':'disabled'}>◀</button><button class="s" data-dmv="1" ${fi>=0&&fi<F.length-1?'':'disabled'}>▶</button></div><label class="f"><span>Eigener Name (leer = ${esc(t.name)})</span><input id="dpName" maxlength="32" value="${esc(t.n||'')}"></label><div class="row"><button class="d" id="dpHide" style="flex:none">Ausblenden</button><button id="dpDone" style="flex:none">Fertig</button></div></div>`:'';
 const H=DP.tiles.filter(t=>!t.on);
 $('#dpHid').innerHTML=H.length?'<h2 style="margin-top:6px">Ausgeblendet</h2>'+H.map(t=>`<div class="dev"><div class="t">${esc(t.n||t.name)}<div class="v">${esc(t.value||'')}</div></div><button data-dshow="${esc(t.k)}">Einblenden</button></div>`).join(''):'';
 $('#dpPer').value=DP.per;$('#dpBright').value=DP.bright;$('#dpDim').value=DP.dim;$('#dpAuto').checked=DP.auto;$('#dpNight').value=DP.night;$('#dpThr').value=DP.ldr_thr}
$('#dpEd').addEventListener('click',e=>{const b=e.target.closest('button');
 if(b&&b.id=='dpAdd'){DP.P.push({n:DP.per,k:[]});dSel=null;return dpRender()}
 if(b&&b.dataset.dpdel){DP.P.splice(+b.dataset.dpdel,1);dSel=null;return dpRender()}
 const el=e.target.closest('.tl');if(!el)return;const at={p:+el.dataset.p,i:+el.dataset.i},has=DP.P[at.p].k[at.i]!=null;
 if(!dSel){if(has)dSel=at}else if(dSel.p==at.p&&dSel.i==at.i)dSel=null;else{dpMove(dSel,at);dSel=null}dpRender()});
$('#dpEd').addEventListener('change',e=>{const d=e.target.dataset;if(d.pl==null)return;DP.P[+d.pl].n=+e.target.value;dpFix();dSel=null;dpRender()});
$('#dpEd').addEventListener('dragstart',e=>{const el=e.target.closest('.tl');if(!el||el.classList.contains('em'))return;dG={p:+el.dataset.p,i:+el.dataset.i};e.dataTransfer.effectAllowed='move';e.dataTransfer.setData('text/plain','x')});
$('#dpEd').addEventListener('dragover',e=>{const el=e.target.closest('.tl');if(!dG||!el)return;e.preventDefault();document.querySelectorAll('.tl.ov').forEach(x=>x.classList.remove('ov'));el.classList.add('ov')});
$('#dpEd').addEventListener('drop',e=>{const el=e.target.closest('.tl');if(!el||!dG)return;e.preventDefault();dpMove(dG,{p:+el.dataset.p,i:+el.dataset.i});dG=null;dSel=null;dpRender()});
$('#dpEd').addEventListener('dragend',()=>{dG=null;document.querySelectorAll('.tl.ov').forEach(x=>x.classList.remove('ov'))});
$('#dpSel').addEventListener('click',e=>{const b=e.target.closest('button');if(!b||!dSel)return;
 if(b.dataset.dmv){const F=dFlat(),fi=F.findIndex(x=>x.p==dSel.p&&x.i==dSel.i),to=F[fi+(+b.dataset.dmv)];if(to){dpMove(dSel,to);dSel=to}}
 if(b.id=='dpHide'){const k=DP.P[dSel.p].k.splice(dSel.i,1)[0];dT(k).on=false;dSel=null}
 if(b.id=='dpDone')dSel=null;dpRender()});
$('#dpSel').addEventListener('input',e=>{if(e.target.id!='dpName'||!dSel)return;const t=dT(DP.P[dSel.p].k[dSel.i]);if(t)t.n=e.target.value});
$('#dpHid').addEventListener('click',e=>{const b=e.target.closest('button');if(!b||!b.dataset.dshow)return;const t=dT(b.dataset.dshow);if(!t)return;t.on=true;
 let L=DP.P[DP.P.length-1];if(L.k.length>=L.n){L={n:DP.per,k:[]};DP.P.push(L)}L.k.push(t.k);dpRender()});
$('#dpPer').onchange=e=>{DP.per=+e.target.value};
// Speichern: leere Seiten fallen weg, jede Kachel bekommt ihre Seite
$('#bDpSave').onclick=()=>{const P=DP.P.filter(p=>p.k.length),tiles=[];P.forEach((p,pi)=>p.k.forEach(k=>{const t=dT(k);tiles.push({k,on:true,n:t.n||'',pg:pi})}));
 DP.tiles.filter(t=>!t.on).forEach(t=>tiles.push({k:t.k,on:false,n:t.n||''}));
 post('/api/display',{tiles,pl:P.map(p=>p.n),per:+$('#dpPer').value,bright:+$('#dpBright').value,dim:+$('#dpDim').value,auto:$('#dpAuto').checked,night:+$('#dpNight').value,ldr_thr:+$('#dpThr').value}).then(()=>{toast('Display gespeichert.');dpLoad()}).catch(e=>toast(e.message,true))};
load();dpLoad();setInterval(load,3000);
</script></body></html>)HTML";
