package io.github.adswill.onair

/** What ModeDef.note means (the numbers are written by tools/gen_modes.py) and its text for the mode list. 0 = nothing to say. */
object ModeNotes {
    const val DESKTOP_ONLY = 1
    fun text(note: Int): Int? = when (note) {
        DESKTOP_ONLY -> R.string.mode_note_desktop
        2 -> R.string.mode_note_picture
        3 -> R.string.mode_note_sound
        4 -> R.string.mode_note_voice
        5 -> R.string.mode_note_image
        6 -> R.string.mode_note_auto
        else -> null
    }
}
