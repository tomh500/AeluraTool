#include "Recording.Memory.h"
#include <wrl/implements.h>
#include <winrt/base.h>
#include <mfapi.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
namespace aelura::recording::media {
constexpr uint64_t blockSize=65536;
bool MemoryBudget::acquire(uint64_t bytes){
    if(bytes>limit)return false;
    if(used.load()>limit-bytes&&reclaim)reclaim(bytes);
    auto current=used.load();
    while(current<=limit-bytes){if(used.compare_exchange_weak(current,current+bytes))return true;}
    return false;
}
struct MemoryStorage {
    std::shared_ptr<MemoryBudget> budget;
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<BYTE[]>> blocks;
    uint64_t length{};bool sealed{};
    explicit MemoryStorage(std::shared_ptr<MemoryBudget> b):budget(std::move(b)){}
    ~MemoryStorage(){auto bytes=blocks.size()*blockSize;blocks.clear();budget->release(bytes);}
    HRESULT grow(uint64_t end){
        if(end>budget->limit)return STG_E_MEDIUMFULL;
        const auto count=(end+blockSize-1)/blockSize;
        try{while(blocks.size()<count){
            if(!budget->acquire(blockSize))return E_OUTOFMEMORY;
            try{blocks.push_back(std::make_unique<BYTE[]>(blockSize));}
            catch(...){budget->release(blockSize);throw;}
        }}catch(...){return E_OUTOFMEMORY;}return S_OK;
    }
};
class MemoryStream final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IStream> {
    std::shared_ptr<MemoryStorage> data_;std::mutex mutex_;uint64_t position_{};
public:
    MemoryStream(std::shared_ptr<MemoryStorage> data,uint64_t position=0):data_(std::move(data)),position_(position){}
    HRESULT STDMETHODCALLTYPE Read(void* buffer,ULONG bytes,ULONG* read) override {
        if(read)*read=0;if(bytes&&!buffer)return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_,data_->mutex);
        const auto count=ULONG(std::min<uint64_t>(bytes,position_>=data_->length?0:data_->length-position_));
        for(ULONG done=0;done<count;){auto length=ULONG(std::min<uint64_t>(count-done,blockSize-position_%blockSize));memcpy(static_cast<BYTE*>(buffer)+done,data_->blocks[size_t(position_/blockSize)].get()+position_%blockSize,length);position_+=length;done+=length;}
        if(read)*read=count;return count==bytes?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(void const* buffer,ULONG bytes,ULONG* written) override {
        if(written)*written=0;if(bytes&&!buffer)return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_,data_->mutex);if(data_->sealed)return STG_E_ACCESSDENIED;
        if(!bytes)return S_OK;
        if(position_>std::numeric_limits<uint64_t>::max()-bytes)return STG_E_MEDIUMFULL;
        auto hr=data_->grow(position_+bytes);if(FAILED(hr))return hr;
        for(ULONG done=0;done<bytes;){auto length=ULONG(std::min<uint64_t>(bytes-done,blockSize-position_%blockSize));memcpy(data_->blocks[size_t(position_/blockSize)].get()+position_%blockSize,static_cast<BYTE const*>(buffer)+done,length);position_+=length;done+=length;}
        data_->length=std::max(data_->length,position_);if(written)*written=bytes;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* result) override {
        std::scoped_lock lock(mutex_,data_->mutex);uint64_t base{};
        if(origin==STREAM_SEEK_CUR)base=position_;else if(origin==STREAM_SEEK_END)base=data_->length;else if(origin!=STREAM_SEEK_SET)return STG_E_INVALIDFUNCTION;
        uint64_t next{};
        if(move.QuadPart<0){auto magnitude=uint64_t(-(move.QuadPart+1))+1;if(base<magnitude)return STG_E_INVALIDFUNCTION;next=base-magnitude;}
        else{if(base>std::numeric_limits<uint64_t>::max()-uint64_t(move.QuadPart))return STG_E_INVALIDFUNCTION;next=base+move.QuadPart;}
        if(next>data_->budget->limit)return STG_E_MEDIUMFULL;position_=next;if(result)result->QuadPart=position_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER length) override {
        std::scoped_lock lock(mutex_,data_->mutex);if(data_->sealed)return STG_E_ACCESSDENIED;
        auto hr=data_->grow(length.QuadPart);if(FAILED(hr))return hr;
        const auto count=(length.QuadPart+blockSize-1)/blockSize;
        if(count<data_->blocks.size()){const auto removed=data_->blocks.size()-size_t(count);data_->blocks.resize(size_t(count));data_->budget->release(removed*blockSize);}
        if(length.QuadPart<data_->length&&length.QuadPart%blockSize)memset(data_->blocks.back().get()+length.QuadPart%blockSize,0,size_t(blockSize-length.QuadPart%blockSize));
        data_->length=length.QuadPart;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination,ULARGE_INTEGER bytes,ULARGE_INTEGER* read,ULARGE_INTEGER* written) override {
        if(read)read->QuadPart=0;if(written)written->QuadPart=0;if(!destination||destination==this)return STG_E_INVALIDPOINTER;
        BYTE buffer[65536];uint64_t total{};
        while(total<bytes.QuadPart){ULONG count{},saved{};auto hr=Read(buffer,ULONG(std::min<uint64_t>(sizeof(buffer),bytes.QuadPart-total)),&count);if(FAILED(hr))return hr;if(!count)break;
            hr=destination->Write(buffer,count,&saved);total+=count;if(read)read->QuadPart=total;if(written)written->QuadPart+=saved;if(FAILED(hr))return hr;if(saved!=count)return STG_E_MEDIUMFULL;
        }return total==bytes.QuadPart?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Revert() override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat,DWORD) override {
        if(!stat)return STG_E_INVALIDPOINTER;std::lock_guard lock(data_->mutex);*stat={};stat->type=STGTY_STREAM;stat->cbSize.QuadPart=data_->length;stat->grfMode=data_->sealed?STGM_READ:STGM_READWRITE;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** result) override {
        if(!result)return E_POINTER;*result=nullptr;std::lock_guard lock(mutex_);auto clone=Microsoft::WRL::Make<MemoryStream>(data_,position_);return clone?clone.CopyTo(result):E_OUTOFMEMORY;
    }
};
MemoryFile::MemoryFile(std::shared_ptr<MemoryBudget> budget):storage_(std::make_shared<MemoryStorage>(std::move(budget))){}
Microsoft::WRL::ComPtr<IStream> MemoryFile::stream()const{auto value=Microsoft::WRL::Make<MemoryStream>(storage_);if(!value)winrt::throw_hresult(E_OUTOFMEMORY);return value;}
Microsoft::WRL::ComPtr<IMFByteStream> MemoryFile::byteStream()const{
    auto source=stream();Microsoft::WRL::ComPtr<IMFByteStream> result;winrt::check_hresult(MFCreateMFByteStreamOnStream(source.Get(),&result));
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;if(SUCCEEDED(result.As(&attributes)))winrt::check_hresult(attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE,L"video/mp4"));return result;
}
uint64_t MemoryFile::size()const{std::lock_guard lock(storage_->mutex);return storage_->length;}
uint64_t MemoryFile::allocated()const{std::lock_guard lock(storage_->mutex);return storage_->blocks.size()*blockSize;}
void MemoryFile::seal(){std::lock_guard lock(storage_->mutex);storage_->sealed=true;}
}
