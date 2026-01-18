# GNeoMutt

**GNeoMutt** est une interface graphique légère (GTK+3) enveloppant le client e-mail en ligne de commande **NeoMutt**. Il intègre un terminal embarqué (VTE) et des commandes de synchronisation pour offrir une expérience fluide entre la puissance du terminal et le confort d'une barre d'outils graphique.

## 🚀 Fonctionnalités

* **Terminal intégré** : Lance NeoMutt directement dans l'interface via la bibliothèque VTE.
* **Barre d'outils rapide** : Boutons dédiés pour la synchronisation, l'envoi de mails, et la navigation.
* **Ressources embarquées** : L'icône et l'interface UI sont compilées directement dans le binaire (GResource).
* **Optimisé** : Compilé avec Clang et les optimisations `-O3`.

## 📦 Prérequis

Avant d'installer GNeoMutt, assurez-vous que les dépendances suivantes sont présentes sur votre système (testé sur Arch Linux) :

### Dépendances de compilation
* `clang` ou `gcc`
* `pkg-config`
* `gtk3`
* `vte3`

### Dépendances d'exécution (outils e-mail)
* `neomutt` : Le client mail principal.
* `isync` (mbsync) : Pour la synchronisation IMAP.
* `notmuch` : Pour l'indexation et la recherche rapide.
* `msmtp` : Pour l'envoi de messages.

```bash
# Installation sur Arch Linux
sudo pacman -S clang gtk3 vte3 neomutt isync notmuch msmtp
```

## 🛠 Installation 

1. **Cloner le dépôt** :

``` bash
git clone git@github.com:albanmartel/gneomutt.git
cd gneomutt
```

2.  **Compiler le projet** : Le Makefile gère la génération des ressources et la compilation optimisée.

``` bash
make
```


3.  **Vérifier les dépendances** : Vérifie que tous les outils sont bien dans votre PATH avant le lancement.

``` bash
make check-deps
```

4.  **Installer sur le système** :

``` bash
sudo make install
```

## ⚙️ Configuration 

GNeoMutt s'appuie sur les outils standards du terminal. Pour que l'application fonctionne avec vos propres comptes e-mails, vous devez adapter les fichiers de configuration fournis en exemple dans le répertoire `.config/`

### Fichiers d\'exemples 

-   **neomuttrc** : Configuration de l\'interface et des macros NeoMutt.

-   **mbsyncrc** : Configuration pour la récupération des mails (IMAP).

-   **msmtprc** : Configuration pour l\'envoi de mails (SMTP).

### Mise en place 

Vous devez copier ces fichiers vers votre répertoire personnel et les éditer avec vos informations :

``` bash
# Exemple pour le fichier principal
cp .config ~
nano ~/neomutt/neomuttrc
```

## 📖 Utilisation

Lancez simplement l'application depuis votre terminal ou votre lanceur d'applications :

``` bash
gneomutt
```


### Raccourcis de l'interface
-   **Sync** : Lance `mbsync -a && notmuch new`
-   **Écrire** : Ouvre le mode rédaction 

-   **Navigation** : Boutons Inbox, Sent, et Archives mappés sur vos macros NeoMutt.

## 🛠 Développement 

-   **Nettoyer les fichiers** : `make clean`

-   **Mode Debug** : `make debug` (compilation avec symboles `-g` )

------------------------------------------------------------------------

Dépôt maintenu par **Alban Martel**.
