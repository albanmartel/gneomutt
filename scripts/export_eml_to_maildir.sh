for f in /chemin/vers/vos/fichiers/*.eml; do
    # Génère un nom unique basé sur l'heure et un hash pour éviter les collisions
    uuid=$(uuidgen | cut -d'-' -f1)
    timestamp=$(date +%s)
    cp "$f" "~/Mail/Archives/cur/${timestamp}.${uuid}:2,S"
done
