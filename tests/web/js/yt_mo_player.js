(function(){
 var log=[]; var t0=Date.now();
 function chain(e){ var s=[],k=0; while(e&&k<8){ s.push(e.tagName.toLowerCase()+(e.id?'#'+e.id:'')); e=e.parentElement; k++; } return s.join('<'); }
 function mark(m){ log.push(((Date.now()-t0)/1000).toFixed(1)+'s '+m); }
 function scan(n){ if(n.nodeType!==1) return; if(n.id==='movie_player') mark('movie_player ADDED '+chain(n)); if(n.id==='player-api') mark('player-api ADDED '+chain(n)); if(n.tagName==='YTD-PLAYER') mark('ytd-player ADDED '+chain(n)); if(n.classList&&n.classList.contains('skeleton')&&n.id==='player') mark('skeleton ADDED'); 
   if(n.querySelector){ var a=n.querySelector('#movie_player'); if(a&&n.id!=='movie_player') mark('movie_player inside added '+chain(a)); } }
 function rem(n){ if(n.nodeType!==1) return; if(n.id==='player'&&n.classList.contains('skeleton')) mark('skeleton REMOVED'); if(n.id==='movie_player') mark('movie_player REMOVED'); if(n.querySelector&&n.querySelector('#movie_player')) mark('subtree with movie_player removed '+n.tagName); }
 var mo=new MutationObserver(function(rs){ rs.forEach(function(r){ r.addedNodes.forEach(scan); r.removedNodes.forEach(rem); }); });
 mo.observe(document,{childList:true,subtree:true});
 window.__traceDump=function(){ return log.join('\n')+'\nFINAL '+chain(document.querySelector('#movie_player'))+' skeleton='+!!document.querySelector('#player.skeleton'); };
})();
