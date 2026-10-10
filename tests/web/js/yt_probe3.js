(function () {
    const v = document.querySelector('video'), q = v.getVideoPlaybackQuality();
    return 'quality total=' + q.totalVideoFrames + ' dropped=' + q.droppedVideoFrames + ' t=' + v.currentTime.toFixed(2) + ' rs=' + v.readyState + ' buffered=' + Array.from({length: v.buffered.length}, (_, i) => v.buffered.start(i).toFixed(1) + '-' + v.buffered.end(i).toFixed(1)).join(',') + ' size=' + v.videoWidth + 'x' + v.videoHeight + ' muted=' + v.muted + ' vol=' + v.volume;
})()
