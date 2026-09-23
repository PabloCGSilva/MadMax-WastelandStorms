# Mad Max – Wasteland Storms (v0.9.0-beta1)

No Mad Max as tempestades só aparecem uma vez a cada 2 a 2,5 horas de jogo,
então a maioria das sessões não tem nenhuma. O **Wasteland Storms** deixa você
escolher a frequência.

Ele usa o próprio comando de tempestade do jogo, então toda tempestade é de
verdade: as mesmas tempestades de areia e de raios, os mesmos destroços, o
mesmo saque no olho da tempestade. O ciclo de tempestades do próprio jogo
continua funcionando por baixo, intocado.

**Esta é uma versão beta.** Faça backup dos seus saves antes de testar.

## Presets

Aperte **F5** no jogo para alternar entre eles. A escolha aparece na tela e
fica salva para a próxima vez.

| Preset | Céu limpo entre tempestades |
|---|---|
| **Off** | só as do jogo |
| **Frequent** | 4 a 9 minutos |
| **Uncommon** | 10 a 30 minutos |
| **Random** | 0 a 60 minutos, podem vir uma atrás da outra |

## Como funciona

* **O intervalo começa quando a tempestade termina de verdade**, não quando
  foi disparada, então elas nunca se sobrepõem. O mod lê o estado do clima do
  jogo para saber quando a tempestade chegou e quando o céu limpou.
* **Só conta tempo de jogo.** Pausa, menus e alt-tab não adiantam a próxima.
* **Ele espera você estar ao ar livre.** Dentro de fortalezas, interiores,
  acampamentos e túneis o jogo nunca cria tempestade, e Gastown e os poços de
  enxofre têm clima próprio. Enquanto você estiver num desses lugares a
  contagem fica parada e continua quando você sai.
* **Dá para fugir de uma tempestade.** Ela é uma frente que se move: chega a
  quem espera parado uns 40 segundos depois de se formar e dura uns 5 minutos
  e meio. Quem dirige para longe rápido o bastante não é alcançado. Se ela não
  chegar em dois minutos, conta como perdida e a próxima é agendada
  normalmente — nunca uma em cima da outra.
* **O jogo escolhe o tipo** (areia ou raios).
* **Nenhum arquivo do jogo é alterado** e os saves não são afetados.

A mensagem na tela usa a fonte da interface do próprio jogo, lida dos arquivos
do seu jogo ao iniciar (o mod não distribui nenhum arquivo do jogo). Ela só
aparece no mundo aberto, nunca dentro de uma fortaleza.

## Requisitos

* Mad Max (2015, PC).
* [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
  do ThirteenAG — uma cópia (`dinput8.dll`) vem junto.

## Instalação

1. Copie o `dinput8.dll` para a pasta do jogo, ao lado do `AVAMain.exe`
   (pule se já usa o ASI loader, por exemplo para o Enhanced Convoys).
2. Copie a pasta `scripts` para a pasta do jogo, ficando
   `<pasta do jogo>\scripts\WastelandStorms.asi` e `WastelandStorms.ini`.
3. Abra o jogo e carregue um save. Quando você estiver ao ar livre, uma
   mensagem mostra o preset atual.

Para desinstalar, apague `scripts\WastelandStorms.asi` (e o `.ini` / `.log`
ao lado).

## Configurações

`scripts\WastelandStorms.ini`:

```ini
[WastelandStorms]
Preset=Frequent        ; Off, Frequent, Uncommon ou Random
Hotkey=0x74            ; tecla que troca o preset (0x74 = F5, 0x75 = F6, ...)
StartupNotice=1        ; 0 esconde a mensagem mostrada ao entrar no mundo
```

## Compatibilidade

* Funciona junto com o **Enhanced Convoys** (os dois podem ficar instalados).
* Mods de textura, modelo e dados: sem conflito, este mod não mexe em arquivo
  do jogo.
* **GOG**: testado. **Steam**: o mod encontra cada função do jogo procurando no
  executável em execução, então deve funcionar, mas ainda não foi testado na
  Steam. Se não achar algo, ele se desliga e avisa — o jogo continua seguro.
  Por favor avise o resultado, com o `scripts\WastelandStorms.log`.

## Limitações conhecidas

* Não dá para escolher o tipo de tempestade; o jogo decide.
* A tempestade permanente da borda do mundo (Big Nothing) é outra coisa e não
  é afetada.

## Problemas

Tudo o que o mod faz fica em `scripts\WastelandStorms.log` (a sessão anterior
fica em `WastelandStorms.previous.log`). As primeiras linhas dizem se cada
função do jogo foi encontrada.
