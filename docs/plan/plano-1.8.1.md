# Plano da 1.8.1

Status: **gate verde na versão final (207 testes, 0 falhas, 3 opcionais pulados), README e notas prontos. Sem commit e sem release.**

Base: `feat/1.8` em `0d55206` (1.8.0 mais documentação). Na auditoria, 202 testes passaram, 0 falharam e 3 foram pulados, sem cobertura do XMX.

## Decisões já tomadas

- O nome continua **MFG Unlock**. A troca para Ayase foi desfeita por decisão do usuário.
- A documentação continua recomendando o XMX.
- O XMX com DLSS-SR fica fora, para não arriscar a vantagem de latência.
- A interface mantém a estrutura atual e a fonte do ReShade. O mock usa outra fonte, só para visualização.
- Azul do tema do ReShade para o que não é status. Verde, amarelo e vermelho só para status.
- Sem FPS e sem gráfico no painel. O Output FPS cap é digitado.
- Crash dump automático fica fora, porque capturar exceções dentro de um jogo com hooks é arriscado.

## Fase 1: crash provado do Bloodborne

**1. Escrita em memória somente leitura no Vulkan** (`native_vulkan.cpp:275`)
- Não escrever se o jogo já pediu `DISALLOWED`.
- Nos outros casos, clonar o início da cadeia `pNext` para os tipos de tamanho conhecido. Tipo desconhecido mantém a cadeia original e registra no log.
- Remover o `Restore`.
- Não preparar o device se o runtime já foi rejeitado.
- Teste: estrutura em página `PAGE_READONLY`.
- Regressão em jogo: Red Dead Redemption 2 e Indiana Jones.
- Pronto quando: o Bloodborne não crashar. Gerar frames nesse jogo não é promessa desta versão.

## Fase 2: riscos reais do XMX

**2. Texturas liberadas em uso** (`xmx_generator.hpp`, `Capture`)
- Fila limitada que só libera a textura depois que a GPU terminou de usá-la.
- Teste: troca de resolução e de modo do DLSS durante o jogo.

**3. Instalação do hook** (`xmx_hook.hpp`, `Slot::Install`)
- Lock compartilhado com `hook::Install`, nova tentativa e verificação da cobertura de threads.
- Registrar o módulo como visto só depois de instalar com sucesso.
- O painel passa a mostrar a contagem real de hooks.
- Candidato à causa do Dying Light 2, ainda não confirmado.

**4. Troca de device** (`Prepare`)
- Comparar também o device e recriar o contexto quando ele mudar.

**5. Vazamento em resize** (`state.retired`)
- Limitar a lista.
- Destruir os contextos antigos pela função interna hoje sem uso (`kDestroyRva`), depois de validá-la no `libxess_fg` 1.3.1.78.
- Se a validação falhar, fica só o limite, e ao atingi-lo o XMX para com mensagem clara em vez de vazar VRAM.

**6. Frames velhos e falhas silenciosas**
- Descartar o par de frames ao retomar depois de uma pausa.
- Todo caminho que deixa de gerar frames registra um motivo e o expõe ao painel.
- Pausa do jogo aparece como "Paused by the game", separada de "Not receiving frames".

**7. Cópia de fallback** (`Evaluate`)
- Conferir tamanho e formato antes do `CopyResource`. Em caso de divergência, registrar e não copiar.

**9. Compatibilidade do XMX**
- scRGB: detectar antes de assumir a geração, para a NVIDIA continuar funcionando, e explicar o motivo no painel.
- `frameCount > maxFrames`: limitar ao máximo aceito pelo modelo, em vez de falhar para sempre, e avisar no painel.

**11. Pontos não verificados**
- Estados de recurso fixos, matriz de view zerada, UI não enviada e subrects ignorados.
- Comparar cada um com os headers da Intel e testar em jogo.
- Só muda o que tiver ganho comprovado e não piorar a latência. O restante fica documentado, sem mudança.

**12. Testes do XMX**
- Separar a lógica de hooks, retirada e estados da parte que depende de GPU, e cobri-la com testes automatizados.

## Fase 3: CUDA

**8. Darktide**
- Perfil de profundidade do Darktide, para não depender de reiniciar depois de aprender `InvertedDepth`.

**10. Robustez**
- Corrigir os acessos sem lock em `native_swapchain.cpp` (`scrgbBuffers`, `exclusiveTarget`).
- `E_FAIL` devolvido ao jogo (`native_bridge.cpp` 178, 188 e 301) e `OUT_OF_DATE` no present com várias swapchains (`vk_host.cpp`): primeiro um teste que reproduza. Só muda se o teste mostrar um comportamento errado, porque o `E_FAIL` é deliberado.

**Dying Light 2 no README**
- Trocar o aviso de crash ao iniciar por "não reproduzido na 1.8.0".

## Fase 4: logs e relato de problemas

- Uma linha por falha, com componente, motivo e código de erro, sem repetir a cada frame.
- Card de problema no topo do painel: o que falhou, o que tentar, "Save support files" e "Copy report".
- Aviso após fechamento inesperado, usando uma marca de sessão, com o texto "may have closed" e o botão "Dismiss".
- "Save support files" copia logs e relatório para `MFG-Support-<data>` e abre a pasta no Explorer.
- Aviso de que os logs podem conter nomes de pastas do PC.

## Fase 5: nome e interface

Só começa depois da aprovação visual dos boards.

**Nome**
- Muda: `NAME` e `DESCRIPTION` do addon, registro do overlay, título do painel, "Enable", rodapés, cabeçalhos dos relatórios, `FileDescription`, README e os dois testes que citam o nome.
- Não muda: nomes de DLL, `MFGAdaUnlock-XeMFG.log`, chaves de configuração, pastas e repositório, para não quebrar instalações existentes.
- Não muda: as citações "MFG Unlocker" em `vendor/`, porque são atribuição de terceiros.
- A verificar: se o ReShade guarda add-ons desativados pelo nome. Se guardar, quem tinha o mod desativado pode encontrá-lo ativo depois da atualização, e isso entra nas notas da versão.

**Tema OLED**
- Fundo preto, cards e seletores como nos boards, sem carregar fonte nova.
- Medir com o painel aberto e fechado para confirmar que não há custo perceptível.

## Validação e release

1. Investigar por que `provider_identity`, `engineering_manifest` e `release_1_4_5_manifest` são pulados.
2. Recompilar e rodar todos os testes no plano High performance, com o monitor ligado, restaurando o Balanced no fim.
3. Teste em jogo:
   - Bloodborne: não crashar.
   - Red Dead Redemption 2 e Indiana Jones: regressão do Vulkan.
   - Cyberpunk: XMX com resolução dinâmica e troca de modo.
   - Darktide: CUDA e XMX, incluindo cutscenes.
4. Atualizar as versões em todos os pontos e escrever `docs/releases/1.8.1.md`.
5. Publicar só com todos os testes passando.

**Depende de terceiros:** o Dying Light 2 depende de Cpt.Hund testar a versão, e o diagnóstico novo deve mostrar a causa. O plano não promete correção para esse jogo.

## Aprovações pendentes

- [x] Visual dos boards (autorizado junto com "pode comecar o plano"): https://claude.ai/artifact/WuwHpcTK3rGpHJMaupcc2R
- [x] Autorização para codar

## O que mudou em relação ao plano

- **Item 1:** o host do Vulkan deixa de ser preparado quando a própria busca dele não acha os arquivos da Intel, e não quando o runtime é rejeitado. A rejeição do D3D12 olha uma pasta só, e o host também aceita `RTXMFG-XeSS`; usar a rejeição quebraria quem instala assim.
- **Item 5:** a função de destruir contexto não foi validada, então vale o plano B: as texturas são liberadas e, após 16 contextos, o XMX para e pede para reiniciar.
- **Item 9 (scRGB):** não dá para detectar antes de assumir a geração, porque o hook entra quando o módulo carrega, antes de o formato existir. O painel explica o motivo e sugere desligar o HDR ou usar o XeMFG.
- **Item 10:** `E_FAIL` e `OUT_OF_DATE` não mudaram, porque não há teste que mostre comportamento errado.
- **Item 11:** não mudou nada. Os headers da Intel não estão no repositório e nenhum jogo mostrou problema.
- **Dying Light 2 no README:** o README não tinha esse aviso; a informação foi para as notas da versão.
- **Tema:** a aba selecionada usa a linha azul em cima, que é o que o ImGui oferece, e não embaixo como no mock.
- **Fonte:** o plano mantinha a fonte do ReShade. Por decisão do usuário, a 1.8.1 define Cascadia Mono na opção `Font` do ReShade para todos, uma vez, só com o campo vazio e se a fonte existir. O ReShade não deixa um addon usar fonte só no próprio painel.
- **Tema:** o fundo preto saiu por decisão do usuário. O fundo é o do ReShade, com cartões e controles em cinza e texto no tamanho do ReShade.
