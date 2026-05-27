#!/bin/bash

# Configuration
LOG_FILE="notmuch_cleanup.log"
LIMIT_PREVIEW=10
MSG_COUNT=0       # Compteur de messages (ID) avec doublons
TOTAL_REMOVED=0   # Compteur total de fichiers en trop

# Initialisation du log
echo "--- Rapport de suppression du $(date) ---" > "$LOG_FILE"
echo "Analyse et nettoyage en cours..."

# Récupérer tous les IDs
notmuch search --output=messages '*' | while read id; do
    file_count=$(notmuch count --output=files "$id")
    
    if [ "$file_count" -gt 1 ]; then
        ((MSG_COUNT++))
        
        # Récupérer les fichiers triés par taille
        mapfile -t files < <(notmuch search --output=files "$id" | xargs ls -S)
        
        keep_file="${files[0]}"
        delete_files=("${files[@]:1}")
        num_to_delete=${#delete_files[@]}
        ((TOTAL_REMOVED += num_to_delete))

        # --- AFFICHAGE ÉCRAN (Aperçu détaillé pour les 10 premiers) ---
        if [ "$MSG_COUNT" -le "$LIMIT_PREVIEW" ]; then
            echo "[$MSG_COUNT] Message-ID: $id"
            echo "  -> Gardé : $keep_file"
            echo "  -> $num_to_delete fichier(s) en doublon à supprimer."
            echo "------------------------------------------------"
        fi

        # --- ÉCRITURE LOG & PROGRESSION ---
        echo "ID: $id ($num_to_delete doublons)" >> "$LOG_FILE"
        for f in "${delete_files[@]}"; do
            echo "  [SUPPR] $f" >> "$LOG_FILE"
            
            # --- ACTION RÉELLE ---
            # Supprimez le '#' ci-dessous pour activer la suppression
            rm "$f"
        done

        # Affichage de la progression en temps réel
        if [ $((MSG_COUNT % 20)) -eq 0 ] && [ "$MSG_COUNT" -gt "$LIMIT_PREVIEW" ]; then
             echo "En cours : $MSG_COUNT messages traités... ($TOTAL_REMOVED fichiers identifiés au total)"
        fi
    fi
done

echo ""
echo "===================================================="
echo " TERMINÉ "
echo " Messages avec doublons traités : $MSG_COUNT"
echo " Total de fichiers en trop identifiés : $TOTAL_REMOVED"
echo " Détails enregistrés dans : $LOG_FILE"
echo "===================================================="
