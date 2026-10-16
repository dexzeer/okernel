(function(){
 function r(sel){ var e=document.querySelector(sel); if(!e) return sel+': none'; var b=e.getBoundingClientRect(), cs=getComputedStyle(e);
  return sel+': x='+Math.round(b.left)+' w='+Math.round(b.width)+' h='+Math.round(b.height)+' disp='+cs.display+' pos='+cs.position+' flex='+cs.flex+' width='+cs.width+' minw='+cs.minWidth+' maxw='+cs.maxWidth+' marginL='+cs.marginLeft+' float='+cs.cssFloat; }
 var out=[ 'vw='+innerWidth, r('ytd-watch-flexy'), r('#columns'), r('#primary'), r('#primary-inner'), r('#player'), r('#secondary'), r('#secondary-inner'), r('#player-container-outer'), r('#player-container-inner'), r('#movie_player')];
 var f=document.querySelector('ytd-watch-flexy'); if(f){ var cs=getComputedStyle(f); out.push('vars sidebar='+cs.getPropertyValue('--ytd-watch-flexy-sidebar-width')+' ratio='+cs.getPropertyValue('--ytd-watch-flexy-width-ratio')+' maxplayer='+cs.getPropertyValue('--ytd-watch-flexy-max-player-width')+' minplayer='+cs.getPropertyValue('--ytd-watch-flexy-min-player-width')+' hratio='+cs.getPropertyValue('--ytd-watch-flexy-height-ratio')); out.push('attrs='+Array.prototype.map.call(f.attributes,function(a){return a.name+'='+a.value}).join(' ').slice(0,400)); }
 out.forEach(function(l){ console.log('COLS '+l); });
})();
