window.__traceDump = function(){
 function r(sel){ var e=document.querySelector(sel); if(!e) return sel+': none'; var b=e.getBoundingClientRect(), cs=getComputedStyle(e);
  return sel+': x='+Math.round(b.left)+' w='+Math.round(b.width)+' h='+Math.round(b.height)+' disp='+cs.display+' pos='+cs.position+' width='+cs.width+' minw='+cs.minWidth+' maxw='+cs.maxWidth+' marginL='+cs.marginLeft; }
 var out=['vw='+innerWidth+' vh='+innerHeight, r('ytd-watch-flexy'), r('#columns'), r('#primary'), r('#primary-inner'), r('#player'), r('#secondary'), r('#secondary-inner'), r('#player-container-outer'), r('#player-container-inner'), r('#movie_player'), r('.html5-video-container'), r('video')];
 var f=document.querySelector('ytd-watch-flexy'); if(f){ var cs=getComputedStyle(f); out.push('vars sidebar='+cs.getPropertyValue('--ytd-watch-flexy-sidebar-width')+' maxplayer='+cs.getPropertyValue('--ytd-watch-flexy-max-player-width')+' style='+f.getAttribute('style')); }
 var mp=document.querySelector('#movie_player'); if(mp) out.push('movie_player style='+mp.getAttribute('style'));
 return out.join('\n');
};
