# Origem deste código

Fonte: https://github.com/supremegamers/external_aosp_aptx (mirror de
https://android-review.googlesource.com/c/platform/packages/modules/Bluetooth/+/2259745,
o patchset com o qual a Qualcomm contribuiu os encoders de aptX e aptX HD
pro AOSP, em março de 2023).

## Licença

Apache License 2.0. Este mirror específico não inclui um arquivo
LICENSE separado nem cabeçalho por-arquivo nos fontes do algoritmo em
si, mas:

- O AOSP como um todo é licenciado sob Apache 2.0 por padrão.
- Os arquivos "wrapper" que a AOSP usa pra carregar esse mesmo encoder
  (`stack/a2dp/a2dp_vendor_aptx_encoder.cc`, do mesmo patchset) trazem
  o cabeçalho explícito "Licensed under the Apache License, Version
  2.0" - conferido diretamente em
  https://android.googlesource.com/platform/system/bt/+/d43a901c579ef08a1b5d7cfe18f759aad0a14f1a/stack/a2dp/a2dp_vendor_aptx_encoder.cc
- A declaração pública da Qualcomm (citada por múltiplas fontes de
  imprensa - Android Police, 9to5Google, XDA, todas de março de 2023)
  confirma: "They are now a part of the AOSP Apache license, and free
  to use" e "the only Qualcomm products included in this release for
  Android are aptX and aptX HD ENCODERS" - ou seja, especificamente o
  ENCODER (o que este projeto usa, sendo um dispositivo source) está
  liberado; o decoder continua proprietário/licenciado.

Diferente do LDAC (que tem um LICENSE/NOTICE explícito no próprio
repositório vendorizado), aqui a base da licença é a política pública
da AOSP + a declaração da Qualcomm, não um arquivo LICENSE dentro deste
mirror específico. Ainda assim, é uma liberação legítima e proposital
do detentor da patente/propriedade intelectual - categoria bem
diferente de projetos como libopenaptx/libfreeaptx, que são engenharia
reversa sem licença e carregam risco real de infração de patente (ver
discussão no chat que originou este projeto).
