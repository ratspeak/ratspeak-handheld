#pragma once
#include "history/HistoryWindow.h"
#include "voice/MemoUi.h"
#include <cstdio>
#include <cstring>

namespace handheld::ui {
using AudioSpan=history::HistoryWindow::Span;
// Generated text exists for peers without audio rendering. Hide only this exact
// complete fallback; titles, captions, unsupported media and wire bytes survive.
inline bool generatedAudioText(const AudioSpan& row,const char* text) {
    if((!row.nativeAudio() && !(row.audioRemoved() && row.audio<0x40)) || row.titleLength || row.sourceOffset || row.more()) return false;
    char expected[40];std::snprintf(expected,sizeof expected,"Voice message (%us)",row.audioSeconds());
    return !std::strcmp(text,expected);
}
inline bool ownsMessageAudio(const memo::Ui* ui,const uint8_t peer[16],const AudioSpan& row) {
    if(!ui || !ui->visible() || !ui->inlinePlayback()) return false;
    const auto& s=ui->status();
    return s.fromMessage && s.counter==row.counter && s.incoming==row.incoming() && !std::memcmp(s.peer,peer,16);
}
inline void messageAudioControl(const memo::Ui* ui,const uint8_t peer[16],const AudioSpan& row,char* out,size_t capacity) {
    unsigned elapsed=0;const char* action="Play";
    if(ownsMessageAudio(ui,peer,row)) {
        const auto phase=ui->status().phase;
        elapsed=ui->elapsedFrames()*40/1000;
        if(phase==memo::Phase::Review && ui->elapsedFrames()*4==ui->status().length && ui->error()==memo::Code::Ok)
            elapsed=row.audioSeconds();
        if(phase==memo::Phase::Playing) action="Pause";
        else if(phase==memo::Phase::Loading || phase==memo::Phase::Pausing) action="Wait";
    }
    std::snprintf(out,capacity,"%s  %u:%02u/%u:%02u",action,elapsed/60,elapsed%60,row.audioSeconds()/60,row.audioSeconds()%60);
}
inline const char* messageAudioError(const memo::Ui* ui,const uint8_t peer[16],const AudioSpan& row) {
    if(!ownsMessageAudio(ui,peer,row) || ui->error()==memo::Code::Ok) return nullptr;
    auto s=ui->status();s.reason=ui->error();return memo::description(s);
}
}
