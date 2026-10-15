# Hoop City (the VR plaza around the big screen)

Original art for Quest 3 Street, built from code in Blender 4.5 (no AI, no league or team logos).

    blender -b --factory-startup --python vegas_scene.py -- night faces out/night 64 1536
    blender -b --factory-startup --python vegas_post.py -- night faces out/night out/final 1536 768

`night` makes `streetmr_loading*.png`, `dusk` makes `streetmr_menu*.png`: the scene with every
switchable light off, plus one picture per light group (`_lights1..6`: marquee chase phases 1-3,
flickering neon, LED words A and B) that the app adds on top while that group is on
(`OpenXRManager::AddAmbientLayers`). They ship in `Source/Android/app/src/main/assets/StreetMR/StreetVR360`.
