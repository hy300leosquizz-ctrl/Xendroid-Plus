# Gears of War 3 — otimização de 2026-11-18

POCO F7 (Adreno 825, Turnip 27.0.0-devel V38, HyperOS), pacote de teste
`xendroid.compose.fork.ui`, build `e4003ba66` (`main` com PR #130). Cena: primeiro
checkpoint jogável do prólogo (pesadelo de Marcus diante da fonte, com a Lancer no HUD),
cerca de 2520 draws, 65 render passes e 6298 tiles transferidos por frame, sem tocar no
aparelho durante as janelas medidas.

## Resumo

Com `log_gpu_frame_time_breakdown` e `log_gpu_frame_time_breakdown_passes` ligados (que
serializam os passes e custam um pouco de fps, mas decompõem cada passe na GPU), em
janelas de 40 s após estabilizar (`pending_pso = 0`):

| Configuração (`0x4D5308AB`) | fps (relógio) | GPU ms/frame | Passe `1280x2048` | Passe `1120x2352` | Misc ms |
|---|---:|---:|---:|---:|---:|
| Base atual (`e4003ba66`, início da série) | 21,21 | 45,20 | 29,17 ms | 3,25 ms | 5,58 |
| Base atual (`e4003ba66`, fim da série, controle térmico) | 21,98 | 44,55 | 29,91 ms | 3,38 ms | 4,66 |
| **Com as novas quirks combinadas** | **29,33** | **28,24** | **15,33 ms** | **1,01 ms** | **4,90** |

Ou seja, **GPU −37,1% (`44,88 → 28,24 ms/frame`, `28,01 ms` no aquecimento)** e subida de
**21,2–22,0 para 29,3–30 fps** (no HUD: `FPS 30 · 33,3 ms`, o teto do próprio jogo nessa
cena, mesmo com os timestamps por passe ligados e SoC a 94 °C), sem regressão visual nem
geométrica nas cinco regiões comparadas (céu/tornado, horizonte, calçamento de pedra em
primeiro plano, fonte/personagens ao centro e HUD da Lancer).

Em 2026-10-05 (`b105`, antes do arredondamento para zero em software nos vertex shaders),
essa mesma cena chegava a 30 fps apenas com `vulkan_depth_4x_as_1x` e
`vulkan_samples_as_pixels_simple_ps`. O arredondamento para zero em software (#99) somou
cerca de 13,6 ms por frame aos ~2220 draws de cena e sombras projetadas da Unreal Engine 3,
derrubando o jogo de volta para ~21–22 fps.

## Isolados na mesma cena (janelas de 40 s)

Contra a média dos dois controles de base (`44,88 ms`, `45,20` no início e `44,55` no fim,
ou contra os vizinhos imediatos `45,20` / `45,24 ms` nos testes intercalados em tempo
real):

| Opção isolada | fps (relógio) | GPU ms/frame | Passe `1280x2048` | Passe `1120x2352` | Delta GPU | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `alu_round_toward_zero = false` | 27,82 (29,49 warm) | 31,25 (30,53 warm) | 18,76 ms | 1,33 ms | **−13,63 ms (−30,4%)** | **Promovida** |
| `spirv_vs_relaxed_math = 1` | 23,68 | 40,80 | 25,53 ms | 3,53 ms | **−4,08 ms (−9,1%)** | **Promovida** |
| `spirv_texture_implicit_lod = true` | 22,02 | 43,46 | 28,97 ms | 3,33 ms | **−1,42 ms (−3,2%)** | **Promovida** |
| `vulkan_texture_load_to_image = true` (intercalado) | 21,79 | 44,54 | 29,17 ms | 3,25 ms | **−0,68 ms** (`misc −0,93 ms`) | **Promovida** |
| `spirv_structured_forward_jumps = true` | 21,45 | 44,90 | 30,23 ms | 3,32 ms | +0,02 ms | Rejeitada (neutra) |
| `vulkan_memexport_barriers_by_range = true` | — | — | — | — | +0,54 ms (`0,0` elididas) | Rejeitada (neutra) |
| `vulkan_samples_as_pixels_coarse` + `vulkan_fragment_shading_rate` | — | — | — | — | −0,05 ms (`6298` tiles) | Rejeitada (neutra) |
| `vulkan_resolve_clear_in_guest_pass = true` (intercalado) | 21,05 | 46,04 | — | — | +0,80 ms | Rejeitada (regressão) |
| `render_target_7e3_as_r11g11b10 = true` | 21,95 | 43,92 | 29,83 ms | 3,36 ms | −0,96 ms | Rejeitada (com perda) |
| `occlusion_query = 'fake'` | 22,45 | 43,15 | 29,68 ms | 3,38 ms | −1,73 ms | Rejeitada (inexata) |

*(Nota: `vulkan_direct_host_resolve` e `vulkan_direct_host_resolve_to_texture` já vêm
ligadas por padrão para todos os títulos em `e4003ba66`; desligar `direct_host_resolve` em
tempo real custa +2,37 ms/frame nesta cena.)*

## Causas

- **RTZ em software (`alu_round_toward_zero = false`, −13,6 ms).** Os vertex shaders da
  Unreal Engine 3 calculam índices de `vfetch`, `a0` e `floor`/`frc` para skinning,
  instancing e sombras projetadas. Como `shaderRoundingModeRTZFloat32` é 0 no Turnip e no
  driver proprietário deste Adreno 825, cada soma e produto sensível passava por
  Dekker/2Sum em software: o passe principal `1280x2048` (~2090 draws) cai de 29,9 para
  18,8 ms e o passe de sombras `1120x2352` (~130 draws) cai de 3,38 para 1,33 ms (−60%).
- **Multiplicação SM3 em vertex shaders (`spirv_vs_relaxed_math = 1`, −4,1 ms).** Sem a
  emulação de `0 * inf/NaN = 0` em cada multiplicação de VS (exata para operandos finitos,
  mantendo posições idênticas entre passes de profundidade/sombra e cor), o passe
  `1280x2048` cai de 29,9 para 25,5 ms isoladamente, e de 18,8 para 15,3 ms somado a
  `alu_round_toward_zero = false` e LOD implícito.
- **LOD implícito (`spirv_texture_implicit_lod = true`, −1,4 ms) e carga direta na imagem
  (`vulkan_texture_load_to_image = true`, −0,68 ms).** As amostras 2D trocam quatro
  derivadas grossas mais `samgq` por um `sam` com LOD do host, e o untile grava direto no
  alias `R32_UINT`/`R32G32_UINT` da imagem (`misc` cai de 5,58 para 4,65 ms nos braços
  intercalados).
- **Por que as opções do Halo: Reach não mudam nada aqui.** O Gears 3 faz apenas 0,1 a 1,7
  draws com memory export por frame e `read_after_export = 0,0` (`VkMemexport: elided=0.0`),
  seus shaders de cena não ficam presos num `OpSwitch` de saltos para frente em loop, e as
  20 sombras 4x já entram na superfície 1x pelas duas quirks anteriores (`6298` tiles
  inalterados).

## Integridade visual

Comparação por região (`2772x1280`, referência `g3-s1-rt/01_base.png`) contra o controle
final `g3-b8-base-end` e o pacote combinado `g3-b7-shd-all`:

| Captura | `foreground_ground` (`dLuma` / `MAE` / `edge`) | `center_subject` (`dLuma` / `MAE` / `edge`) | `hud_top_right` (`MAE` / `edge`) |
|---|---|---|---|
| `01_base.png` (referência) | `+0,00` / `0,00` / `6,48` | `+0,00` / `0,00` / `4,96` | `0,00` / `12,79` |
| `01_base_end.png` (controle final) | `+0,11` / `3,53` / `6,47` | `+2,67` / `9,49` / `4,97` | `10,66` / `12,68` |
| `01_rtzoff.png` (`alu_round_toward_zero=false`) | `+4,13` / `6,33` / `6,75` | `+19,49` / `24,95` / `5,20` | `9,76` / `12,78` |
| `01_vs1.png` (`spirv_vs_relaxed_math=1`) | `+2,75` / `6,67` / `6,31` | `+6,61` / `11,05` / `4,89` | `9,08` / `12,90` |
| `01_ilod.png` (`spirv_texture_implicit_lod=true`) | `+1,36` / `4,77` / `6,60` | `+8,53` / `14,82` / `5,12` | `11,26` / `12,88` |
| `01_shd_all.png` (combinado) | `+0,88` / `5,06` / `6,38` | `+3,67` / `10,49` / `4,94` | `7,71` / `12,76` |

*(O tornado de cinzas e destroços ao fundo gira continuamente na cena, causando variação
natural de luminância no céu e na silhueta central entre instantes distintos; no piso de
pedra, na fonte, nos personagens e no HUD não há artefato geométrico, vértice explodindo
nem z-fighting. Em `occlusion_query = 'fake'`, surgiu brilho extra de flare no topo do céu,
mais um motivo para mantê-la fora das quirks já que o jogo atinge 30 fps com `'fast'`.)*

## Não testado

- **Campanha longa, veículos, combate pesado e multiplayer sem RTZ em software.** Assim
  como no Halo: Reach, se aparecer geometria piscando ou esticando em outro capítulo do
  Gears of War 3, `alu_round_toward_zero = false` é a primeira quirk a suspeitar.
- **Interação com o estreitamento de sensibilidade do RTZ em software (PR #135).** As
  medições acima foram feitas sobre `e4003ba66` antes do merge do PR #135.
- **Medição sem `log_gpu_frame_time_breakdown` (`plain`).** Como `g3-b7-shd-all` já atingiu
  29,33 fps (HUD `FPS 30 · 33,3 ms`) mesmo pagando o custo dos timestamps por passe e a
  bateria chegou ao piso de segurança (19% fora da tomada) ao fim de `g3-b8-base-end`, a
  rodada sem timestamps e os próximos títulos da fila (Red Dead Redemption `5454082B`,
  Naruto `555307E5`, Forza Horizon `4D5309C9`/`4D530AA4` e Crysis 3 `4541098E`) ficam para
  a próxima sessão com o aparelho carregado.
