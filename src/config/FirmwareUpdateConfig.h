#pragma once

namespace FirmwareUpdateConfig {

// Cole aqui a URL direta do arquivo firmware.bin publicado no GitHub.
// Exemplo:
// https://github.com/usuario/repositorio/releases/download/v1.0.0/firmware.bin
static constexpr const char* kFirmwareBinUrl = "";

static constexpr const char* kCurrentVersion = "DEV";

// Para testes com GitHub fica pratico usar TLS sem certificado fixo.
// Antes de distribuir, o ideal e trocar para validacao com certificado/assinatura.
static constexpr bool kAllowInsecureTls = true;
static constexpr const char* kRootCaPem = "";

}  // namespace FirmwareUpdateConfig
