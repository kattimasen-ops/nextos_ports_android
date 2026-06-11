#!/bin/sh
# Cuphead Mobile -> Mali-450. CONFIG VENCEDORA s10: BOOT COMPLETO ATE A TELA DE TITULO.
# $PC=9 (Cuphead.Init) e flaky ~1/3 -> use ./go-title.sh p/ retry automatico.
cd /storage/roms/cuphead-recon
fuser -k /storage/roms/cuphead-recon/cuphead 2>/dev/null; sleep 2
export CUP_NOEXTRACT=1 CUP_FORCEIL2=1 CUP_NOSIGINST=1 CUP_NOSIGH=1
export CUP_FORCEINTEG=1 CUP_CLAMPSIG=1 CUP_SIGCLAMP=4096
export CUP_TEXHALF=512 CUP_FORCESTARTCR=1 CUP_SAPATH=/storage/cuphead-sa
export CUP_NOREFRESHDLC=1 CUP_TAPINPUT=1 CUP_TAPSTART=250 CUP_CRSPY=1 CUP_FRAMES=0
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib
nohup ./cuphead > run.out 2>&1 &
echo "PID $!"
