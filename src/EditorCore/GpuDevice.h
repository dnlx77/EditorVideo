#pragma once

#include <memory>
#include <string>

namespace editor::core {

// Che scheda video si sta usando per comporre (per le impostazioni e le statistiche).
struct GpuInfo
{
    std::string adapterName;  // es. «NVIDIA GeForce RTX 5060 Ti»
    bool software = false;    // rasterizzatore software (WARP / Microsoft Basic Render Driver): solo per le prove
    int featureLevel = 0;     // 110 = D3D 11_0, 111 = 11_1
};

// Il dispositivo Direct3D 11 su cui girano gli shader del compositore. È l'adattatore predefinito, lo stesso su cui
// FFmpeg apre D3D11VA per decodificare. Non è thread-safe: il contesto immediato lo usa un thread alla volta.
// Solo Windows; sugli altri sistemi create() dà sempre nullptr (si compone in CPU).
class GpuDevice
{
public:
    // Il dispositivo, o nullptr se non c'è una scheda utilizzabile (feature level 11_0 e shader compilabili). Con
    // `allowSoftware` vanno bene anche WARP e il Basic Render Driver (le prove in CI non hanno una GPU vera): nell'uso
    // normale è false, così senza GPU si passa alla CPU, che è più veloce di un rasterizzatore software.
    // `error`, se dato, riceve il motivo del rifiuto.
    static std::unique_ptr<GpuDevice> create(bool allowSoftware = false, std::string* error = nullptr);
    ~GpuDevice();
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    const GpuInfo& info() const { return info_; }

    struct Impl;  // D3D11, vedi GpuInternal.h
    Impl& impl() { return *impl_; }

private:
    GpuDevice();
    std::unique_ptr<Impl> impl_;
    GpuInfo info_;
};

}  // namespace editor::core
