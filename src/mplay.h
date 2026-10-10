#ifndef MPLAY_H
#define MPLAY_H

// Media player core: WebM (Opus audio) from a URL to the HD Audio output.
int  mplay_start(const char* url);   // http:// or https://; 0 if the download started
void mplay_stop(void);
void mplay_poll(void);               // main loop: finish the download, decode a few packets
int  mplay_active(void);
int  mplay_wants_cpu(void);          // keep the main loop from halting while playing

#endif
