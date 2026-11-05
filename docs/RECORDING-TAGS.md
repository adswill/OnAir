# Recording tags and notes

In the **Recording library** (Library button in the diagnostics row) every recording has a **Tags** and a **Note** column. **Edit** opens a small
window: tags separated by commas (`airband, night, ISS`), and a free note. Tags are lower-cased and de-duplicated.

**Search** box above the table: type words and the table keeps the recordings where every word is found in the file name, a tag or the note
(case-insensitive). `#tag` matches that tag exactly. **Advanced** shows the tags in use as one-click filters (with how many recordings carry each)
and **Clear filter**.

Where it is kept: one small text file in the recordings folder, `.onair-tags.tsv` (file name, tags, note, tab separated). It goes with the folder
when you move it and never changes the recordings. Renaming a recording outside OnAir detaches its tags (edit the file name in the .tsv).

Code: `app/state/rec_tags.cpp` (pure), `app/panels/recording_library_ui.cpp`. Test: `rec_tags` (`tests/app/test_rec_tags.cpp`).
