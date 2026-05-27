#!/bin/bash

# Utilisation : ./script.sh <dossier_source_eml> <dossier_destination_maildir>
SRC_DIR="$1"
DEST_DIR="$2"

if [ -z "$SRC_DIR" ] || [ -z "$DEST_DIR" ]; then
    echo "Usage: $0 <dossier_source> <dossier_destination>"
    exit 1
fi

# 1. Création de la structure Maildir
echo "--- Initialisation de la structure Maildir dans $DEST_DIR ---"
mkdir -p "$DEST_DIR"/{cur,new,tmp}

# 2. Indexation temporaire pour détecter les doublons dans la source
# On utilise un dossier de base de données temporaire pour ne pas polluer l'officielle
export NOTMUCH_CONFIG="$DEST_DIR/.notmuch_config"
notmuch setup --database="$SRC_DIR" --user.name="Import" --user.primary_email="import@local"

echo "Analyse des messages dans $SRC_DIR..."
notmuch new > /dev/null

# 3. Traitement et Importation
echo "Démarrage de l'importation (sélection du plus gros fichier par Message-ID)..."

MSG_COUNT=0
IMPORT_COUNT=0

# On liste les Message-IDs uniques
notmuch search --output=messages '*' | while read id; do
    ((MSG_COUNT++))
    
    # Trouver tous les fichiers pour cet ID, triés par taille (le plus gros en premier)
    # On prend le premier de la liste
    best_file=$(notmuch search --output=files "$id" | xargs ls -S | head -n 1)
    
    if [ -f "$best_file" ]; then
        # Génération d'un nom de fichier standard Maildir
        # Format : timestamp.unique_id.hostname:2,S
        ts=$(date +%s)
        uuid=$(cat /proc/sys/kernel/random/uuid | cut -d'-' -f1)
        new_name="${ts}.${uuid}.${HOSTNAME}:2,S"
        
        cp "$best_file" "$DEST_DIR/cur/$new_name"
        ((IMPORT_COUNT++))
        
        # Affichage de progression
        if [ $((IMPORT_COUNT % 50)) -eq 0 ]; then
            echo "Progression : $IMPORT_COUNT messages importés..."
        fi
    fi
done

# Nettoyage de la config temporaire Notmuch
rm "$DEST_DIR/.notmuch_config"
rm -rf "$SRC_DIR/.notmuch"

echo ""
echo "===================================================="
echo " IMPORTATION TERMINÉE "
echo " Messages sources analysés : $MSG_COUNT"
echo " Messages uniques importés dans $DEST_DIR/cur/ : $IMPORT_COUNT"
echo "===================================================="
echo "Vous pouvez maintenant lancer : notmuch new"
