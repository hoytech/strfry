## Following secret key is just a junk testing key
nostril --envelope --sec c1eee22f68dc218d98263cfecb350db6fc6b3e836b47423b66c62af7ae3e32bb --content "$(perl -MYAML -MJSON::XS -E 'print encode_json(YAML::LoadFile(q{homepage-community.yaml}));')" --kind 33700 --tag d homepage | websocat ws://127.0.0.1:7777
