# Immortals : fermeture du décodeur VP9

Correctif spécifique au titre 01004A600EC0A000, module main de build ID
70F3F6751D73C644BCE904CCB414E35B00000000000000000000000000000000
(version utilisée dans la capture, mise à jour v0.11.0).

La fermeture du worker copie son ThreadType sur la pile avant JoinThread.
Si le worker possède le mutex interne pendant cette copie, sa terminaison libère
l'original mais laisse la copie verrouillée. JoinThread sur la copie peut alors
attendre indéfiniment. La capture montre précisément cette situation pour le
thread `VP9 Async support decoder` : original libre, copie sur pile détenue par
le thread terminé, thread appelant bloqué dans WaitForAddress.

Le patch réordonne neuf instructions : JoinThread sur l'original, copie de
ThreadType après terminaison, DestroyThread sur la copie. Le chemin de
libération de la pile allouée reste identique. Aucune temporisation, retour
succès fictif ou modification globale du noyau n'est introduit.

Ce patch IPS est chargé par le système de mods existant et ne modifie pas les
fichiers du jeu. Seul ce build ID est sélectionné par le chargeur. La plage
modifiée est 36 octets à l'offset 0x4b4240 de l'image main décompressée
(offset IPS 0x4b4340, en incluant le header NSO de 0x100 octets).

Validation :
- Instructions du site comparées à la capture avant génération du patch.
- Exécution ARM64 du site et des instructions SDK réelles de JoinThread,
  acquisition et libération de mutex dans Unicorn 2.1.4.
- 240 combinaisons de terminaison pendant la copie, mutex détenu/libre et
  nettoyage TLS : correctif 240/240 ; original 10 blocages, 230 réussites.
- Un JoinThread, une copie de 456 octets et un DestroyThread dans les deux cas.
  Contrôle sans blocage : 65 instructions exécutées dans les deux versions.
  Ce comptage ne mesure pas les performances réelles de l'émulateur.
- Registres préservés, pointeur d'allocation de pile conservé.
- Parseur IPS extrait du code de production : les 36 octets sont appliqués
  exactement, les octets voisins restent inchangés, patch tronqué refusé.

Limites : les opérations de fin du worker, de copie mémoire et de destruction
sont simulées ; les instructions de mutex et du chemin JoinThread sont réelles.
Ce test établit la suppression de la course identifiée, mais n'est pas un run
du jeu dans NXEmu. Des relancements en jeu restent nécessaires, notamment pour
vérifier les cinématiques et l'absence d'effets secondaires dans les autres
workers qui utilisent cette fonction de fermeture.

La capture ne prouve pas pourquoi le timing de l'émulateur expose cette course,
ni que les vérifications réseau sont rapides. Ce correctif ne traite pas les
vérifications réseau longues ou les shaders bindless en erreur.

Reproduction depuis la racine du dépôt :
```
<Python> tests/immortals-vp9/test_arm64.py build/pr396-corrections/immortal-freeze/hang_10220_20261009_222445.dmp
<Python> tests/immortals-vp9/test_ips.py
powershell -File tests/immortals-vp9/test_ips.ps1
```
Installer Unicorn et Capstone dans
`build/pr396-corrections/immortal-freeze/python-deps`.
La capture mémoire reste locale et n'est pas distribuée avec ce correctif.
