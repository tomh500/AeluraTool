#include "Recording.Media.h"
#include <winrt/base.h>
#include <mferror.h>
#include <algorithm>
#include <array>
namespace aelura::recording {
using namespace media;
using winrt::check_hresult;
static void joinReaders(size_t count,std::function<void(size_t,Ptr<IMFSourceReader>&)> const& create,std::filesystem::path const& output,std::function<bool(double)> progress,bool discardFailure=false) {
    if(!count) winrt::throw_hresult(E_INVALIDARG);
    Runtime runtime;
    // Reserve the output atomically. MF opens this exact file; never overwrite a caller's file.
    HANDLE reserved=CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(reserved==INVALID_HANDLE_VALUE) winrt::throw_last_error(); CloseHandle(reserved);
    struct Cleanup {std::filesystem::path path;bool remove;~Cleanup(){if(remove){std::error_code error;std::filesystem::remove(path,error);}}} cleanup{output,discardFailure};
    Ptr<IMFSinkWriter> writer; LONGLONG offset=0;
    std::array<DWORD,2> destinations{MAXDWORD,MAXDWORD};
    std::array<Ptr<IMFMediaType>,2> types;
    size_t completed=0;
    while(completed<count) {
        if(progress&&!progress(double(completed)/count))winrt::throw_hresult(E_ABORT);
        Ptr<IMFSourceReader> reader;create(completed,reader);
        check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));
        std::array<DWORD,2> sources{MAXDWORD,MAXDWORD};
        for(DWORD i=0;i<8;++i) {
            Ptr<IMFMediaType> type; auto hr=reader->GetNativeMediaType(i,0,&type);
            if(hr==MF_E_INVALIDSTREAMNUMBER) break; check_hresult(hr);
            GUID major{}; check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE,&major));
            const int slot=major==MFMediaType_Video?0:major==MFMediaType_Audio?1:-1;
            if(slot<0) continue;
            sources[slot]=i; check_hresult(reader->SetCurrentMediaType(i,nullptr,type.Get())); check_hresult(reader->SetStreamSelection(i,TRUE));
            if(!writer) types[slot]=type;
            else {
                GUID before{},after{}; check_hresult(types[slot]->GetGUID(MF_MT_SUBTYPE,&before)); check_hresult(type->GetGUID(MF_MT_SUBTYPE,&after));
                if(before!=after) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
            }
        }
        if(sources[0]==MAXDWORD) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
        if(!writer) {
            Ptr<IMFAttributes> attrs; check_hresult(MFCreateAttributes(&attrs,1)); check_hresult(attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,TRUE));
            check_hresult(MFCreateSinkWriterFromURL(output.c_str(),nullptr,attrs.Get(),&writer));
            for(unsigned i=0;i<2;++i) if(types[i]) { check_hresult(writer->AddStream(types[i].Get(),&destinations[i])); check_hresult(writer->SetInputMediaType(destinations[i],types[i].Get(),nullptr)); }
            check_hresult(writer->BeginWriting());
        }
        if((sources[1]!=MAXDWORD)!=(destinations[1]!=MAXDWORD)) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
        LONGLONG videoEnd=0; std::array<bool,2> ended{false,sources[1]==MAXDWORD};
        while(!ended[0] || !ended[1]) {
            if(progress&&!progress(double(completed)/count))winrt::throw_hresult(E_ABORT);
            DWORD stream{},flags{}; LONGLONG time{}; Ptr<IMFSample> s;
            check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_ANY_STREAM),0,&stream,&flags,&time,&s));
            if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
            const unsigned slot=stream==sources[0]?0:1;
            if(flags & MF_SOURCE_READERF_ENDOFSTREAM) ended[slot]=true;
            if(!s) continue;
            LONGLONG duration{}; check_hresult(s->GetSampleDuration(&duration));
            if(slot==0) videoEnd=std::max(videoEnd,time+duration);
            check_hresult(s->SetSampleTime(std::max(0LL,time)+offset));
            UINT64 decode{}; if(SUCCEEDED(s->GetUINT64(MFSampleExtension_DecodeTimestamp,&decode))) check_hresult(s->SetUINT64(MFSampleExtension_DecodeTimestamp,decode+offset));
            check_hresult(writer->WriteSample(destinations[slot],s.Get()));
        }
        if(videoEnd<=0) winrt::throw_hresult(MF_E_INVALID_FILE_FORMAT);
        offset+=videoEnd;
        ++completed;
    }
    check_hresult(writer->Finalize());
    cleanup.remove=false;
    if(progress)progress(1);
}
void join(std::vector<std::filesystem::path> const& files,std::filesystem::path const& output,std::function<bool(double)> progress){
    joinReaders(files.size(),[&](size_t index,Ptr<IMFSourceReader>& reader){check_hresult(MFCreateSourceReaderFromURL(files[index].c_str(),nullptr,&reader));},output,std::move(progress));
}
void media::joinMemory(std::vector<std::shared_ptr<MemoryFile>> const& files,std::filesystem::path const& output,std::function<bool(double)> progress){
    joinReaders(files.size(),[&](size_t index,Ptr<IMFSourceReader>& reader){auto stream=files[index]->byteStream();check_hresult(MFCreateSourceReaderFromByteStream(stream.Get(),nullptr,&reader));},output,std::move(progress),true);
}
}
