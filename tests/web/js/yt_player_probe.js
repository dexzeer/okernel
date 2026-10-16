(function(){
 var p=document.querySelector('#ytd-player');
 var bc=ytplayer.bootstrapPlayerContainer; console.log('PL3 bootstrapContainer='+(bc?bc.tagName+'#'+bc.id+' parent='+(bc.parentElement&&(bc.parentElement.tagName+'#'+bc.parentElement.id)):'null'));
 try{ console.log('PL3 getPlayer='+(p.getPlayer()?'obj':'null')); }catch(e){ console.log('PL3 getPlayer threw '+e); }
 try{ p.getPlayerPromise().then(function(x){ console.log('PL3 getPlayerPromise resolved '+(x?'obj':'null')); }, function(e){ console.log('PL3 getPlayerPromise rejected '+e); }); }catch(e){ console.log('PL3 gpp threw '+e); }
 try{ p.getMainAppPlayerPromise().then(function(x){ console.log('PL3 mainApp resolved '+(x?'obj':'null')); }, function(e){ console.log('PL3 mainApp rejected '+e); }); }catch(e){ console.log('PL3 gmp threw '+e); }
 console.log('PL3 useMainAppPlayer='+(p.useMainAppPlayer&&p.useMainAppPlayer())+' wiz='+(p.useWizPlayerComponent&&p.useWizPlayerComponent()));
 var f=document.querySelector('ytd-watch-flexy'); console.log('PL3 flexy keys='+Object.keys(f).filter(function(k){return /player/i.test(k)}).join(','));
 var c=p.querySelector('#container'); console.log('PL3 container kids='+(c?c.children.length:'none'));
})();
