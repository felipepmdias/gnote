/*
 * gnote
 *
 * Copyright (C) 2010-2013 Aurimas Cernius
 * Copyright (C) 2010 Debarshi Ray
 * Copyright (C) 2009 Hubert Figuiere
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <exception>

#include <boost/bind.hpp>
#include <boost/format.hpp>

#include <glib.h>
#include <glibmm/i18n.h>
#include <gtkmm/main.h>

#include "applicationaddin.hpp"
#include "debug.hpp"
#include "notemanager.hpp"
#include "notewindow.hpp"
#include "addinmanager.hpp"
#include "ignote.hpp"
#include "itagmanager.hpp"
#include "trie.hpp"
#include "sharp/directory.hpp"
#include "sharp/exception.hpp"
#include "sharp/files.hpp"
#include "sharp/uuid.hpp"
#include "sharp/string.hpp"
#include "sharp/datetime.hpp"
#include "notebooks/notebookmanager.hpp"

namespace gnote {

  class TrieController
  {
  public:
    TrieController(NoteManager &);
    ~TrieController();

    void add_note(const Note::Ptr & note);
    void update();
    TrieTree<Note::WeakPtr> *title_trie() const
      {
        return m_title_trie;
      }
  private:
    void on_note_added (const Note::Ptr & added);
    void on_note_deleted (const Note::Ptr & deleted);
    void on_note_renamed (const Note::Ptr & renamed, const std::string & old_title);
      
    NoteManager & m_manager;
    TrieTree<Note::WeakPtr> *    m_title_trie;
  };

  bool compare_dates(const Note::Ptr & a, const Note::Ptr & b)
  {
    return (a->change_date() > b->change_date());
  }

  NoteManager::NoteManager(const std::string & directory)
  {
    std::string backup = directory + "/Backup";
    
    _common_init(directory, backup);
  }


  void NoteManager::_common_init(const std::string & directory, const std::string & backup_directory)
  {
    m_addin_mgr = NULL;
    m_trie_controller = NULL;

    Glib::RefPtr<Gio::Settings> settings = Preferences::obj()
      .get_schema_settings(Preferences::SCHEMA_GNOTE);
    // Watch the START_NOTE_URI setting and update it so that the
    // StartNoteUri property doesn't generate a call to
    // Preferences.Get () each time it's accessed.
    m_start_note_uri = settings->get_string(Preferences::START_NOTE_URI);
    settings->signal_changed().connect(sigc::mem_fun(*this, &NoteManager::on_setting_changed));
    m_default_note_template_title = _("New Note Template");


    DBG_OUT("NoteManager created with note path \"%s\".", directory.c_str());

    m_notes_dir = directory;
    m_backup_dir = backup_directory;

    bool is_first_run = first_run ();
    create_notes_dir ();

    const std::string old_note_dir = IGnote::old_note_dir();
    const bool migration_needed
                 = is_first_run
                   && sharp::directory_exists(old_note_dir);

    if (migration_needed) {
      migrate_notes(old_note_dir);
      is_first_run = false;
    }

    m_trie_controller = create_trie_controller ();
    m_addin_mgr = create_addin_manager ();

    if (is_first_run) {
      std::list<ImportAddin*> l;
      m_addin_mgr->get_import_addins(l);
      bool has_imported = false;

      if(l.empty()) {
        DBG_OUT("no import plugins");
      }

      for(std::list<ImportAddin*>::iterator iter = l.begin();
          iter != l.end(); ++iter) {

        DBG_OUT("importing");
        (*iter)->initialize();
        if((*iter)->want_to_run(*this)) {
          has_imported |= (*iter)->first_run(*this);
        }
      }
      // we MUST call this after import
      post_load();

      // First run. Create "Start Here" notes.
      create_start_notes ();
    } 
    else {
      load_notes ();
    }

    IGnote::obj().signal_quit.connect(sigc::mem_fun(*this, &NoteManager::on_exiting_event));
  }

  NoteManager::~NoteManager()
  {
    delete m_trie_controller;
    delete m_addin_mgr;
  }

  void NoteManager::on_setting_changed(const Glib::ustring & key)
  {
    if(key == Preferences::START_NOTE_URI) {
      m_start_note_uri = Preferences::obj()
        .get_schema_settings(Preferences::SCHEMA_GNOTE)->get_string(Preferences::START_NOTE_URI);
    }
  }


  // Create the TrieController. For overriding in test methods.
  TrieController *NoteManager::create_trie_controller()
  {
    return new TrieController(*this);
  }

  AddinManager *NoteManager::create_addin_manager()
  {
    return new AddinManager(*this, IGnote::conf_dir());
  }

  // For overriding in test methods.
  bool NoteManager::directory_exists(const std::string & directory) const
  {
    return sharp::directory_exists(directory);
  }

  // For overriding in test methods.
  bool NoteManager::create_directory(const std::string & directory) const
  {
    return g_mkdir_with_parents(directory.c_str(), S_IRWXU) == 0;
  }

  bool NoteManager::first_run() const
  {
    return !directory_exists(m_notes_dir);
  }

  // Create the notes directory if it doesn't exist yet.
  void NoteManager::create_notes_dir() const
  {
    if (!directory_exists(m_notes_dir)) {
      // First run. Create storage directory.
      create_directory(m_notes_dir);
    }
    if (!directory_exists(m_backup_dir)) {
      create_directory(m_backup_dir);
    }
  }
  

  void NoteManager::on_note_rename (const Note::Ptr & note, const std::string & old_title)
  {
//    if (NoteRenamed != null)
    signal_note_renamed(note, old_title);
    m_notes.sort(boost::bind(&compare_dates, _1, _2));
  }

  void NoteManager::on_note_save (const Note::Ptr & note)
  {
//    if (NoteSaved != null)
    signal_note_saved(note);
    m_notes.sort(boost::bind(&compare_dates, _1, _2));
  }

  void NoteManager::create_start_notes ()
  {
    // FIXME: Delay the creation of the start notes so the panel/tray
    // icon has enough time to appear so that Tomboy.TrayIconShowing
    // is valid.  Then, we'll be able to instruct the user where to
    // find the Tomboy icon.
    //string icon_str = Tomboy.TrayIconShowing ?
    //     Catalog.GetString ("System Tray Icon area") :
    //     Catalog.GetString ("GNOME Panel");

    // for some reason I have to set the xmlns -- Hub
    std::string start_note_content =
      _("<note-content xmlns:link=\"http://beatniksoftware.com/tomboy/link\">"
        "Start Here\n\n"
        "<bold>Welcome to Gnote!</bold>\n\n"
        "Use this \"Start Here\" note to begin organizing "
        "your ideas and thoughts.\n\n"
        "You can create new notes to hold your ideas by "
        "selecting the \"Create New Note\" item from the "
        "Gnote menu in your GNOME Panel. "
        "Your note will be saved automatically.\n\n"
        "Then organize the notes you create by linking "
        "related notes and ideas together!\n\n"
        "We've created a note called "
        "<link:internal>Using Links in Gnote</link:internal>.  "
        "Notice how each time we type <link:internal>Using "
        "Links in Gnote</link:internal> it automatically "
        "gets underlined?  Click on the link to open the note."
        "</note-content>");

    std::string links_note_content =
      _("<note-content>"
        "Using Links in Gnote\n\n"
        "Notes in Gnote can be linked together by "
        "highlighting text in the current note and clicking"
        " the <bold>Link</bold> button above in the toolbar.  "
        "Doing so will create a new note and also underline "
        "the note's title in the current note.\n\n"
        "Changing the title of a note will update links "
        "present in other notes.  This prevents broken links "
        "from occurring when a note is renamed.\n\n"
        "Also, if you type the name of another note in your "
        "current note, it will automatically be linked for you."
        "</note-content>");

    try {
      Note::Ptr start_note = create (_("Start Here"),
                                start_note_content);
      start_note->queue_save (CONTENT_CHANGED);
      Preferences::obj().get_schema_settings(Preferences::SCHEMA_GNOTE)->set_string(
          Preferences::START_NOTE_URI, start_note->uri());

      Note::Ptr links_note = create (_("Using Links in Gnote"),
                                links_note_content);
      links_note->queue_save (CONTENT_CHANGED);
    } 
    catch (const std::exception & e) {
      ERR_OUT(_("Error creating start notes: %s"), e.what());
    }
  }


  void NoteManager::add_note(const Note::Ptr & note)
  {
    if (note) {
      note->signal_renamed().connect(sigc::mem_fun(*this, &NoteManager::on_note_rename));
      note->signal_saved().connect(sigc::mem_fun(*this, &NoteManager::on_note_save));
      m_notes.push_back(note);
    }
  }
  
  void NoteManager::load_notes()
  {
    std::list<std::string> files;
    sharp::directory_get_files_with_ext(m_notes_dir, ".note", files);

    for(std::list<std::string>::const_iterator iter = files.begin();
        iter != files.end(); ++iter) {
      const std::string & file_path(*iter);
      try {
        Note::Ptr note = Note::load(file_path, *this);
        add_note(note);
      } 
      catch (const std::exception & e) {
        /* TRANSLATORS: first %s is file, second is error */
        ERR_OUT(_("Error parsing note XML, skipping \"%s\": %s"),
                file_path.c_str(), e.what());
      }
    }
    post_load();
    // Make sure that a Start Note Uri is set in the preferences, and
    // make sure that the Uri is valid to prevent bug #508982. This
    // has to be done here for long-time Tomboy users who won't go
    // through the create_start_notes () process.
    if (start_note_uri().empty() ||
        !find_by_uri(start_note_uri())) {
      // Attempt to find an existing Start Here note
      Note::Ptr start_note = find (_("Start Here"));
      if (start_note) {
        Preferences::obj().get_schema_settings(Preferences::SCHEMA_GNOTE)->set_string(
            Preferences::START_NOTE_URI, start_note->uri());
      }
    }
  }


  void NoteManager::post_load()
  {
    m_notes.sort (boost::bind(&compare_dates, _1, _2));

    // Update the trie so addins can access it, if they want.
    m_trie_controller->update ();

    // Load all the addins for our notes.
    // Iterating through copy of notes list, because list may be
    // changed when loading addins.
    Note::List notesCopy(m_notes);
    for(Note::List::const_iterator iter = notesCopy.begin();
        iter != notesCopy.end(); ++iter) {
      const Note::Ptr & note(*iter);

      m_addin_mgr->load_addins_for_note (note);
    }
  }

  void NoteManager::migrate_notes(const std::string & old_note_dir)
  {
    std::list<std::string> files;
    sharp::directory_get_files_with_ext(old_note_dir, ".note", files);

    for (std::list<std::string>::const_iterator iter = files.begin();
         files.end() != iter; ++iter) {
      const Glib::RefPtr<Gio::File> src = Gio::File::create_for_path(
                                                       *iter);
      const std::string dest_path
          = Glib::build_filename(m_notes_dir,
                                 Glib::path_get_basename(*iter));
      const Glib::RefPtr<Gio::File> dest = Gio::File::create_for_path(
                                                        dest_path);
      src->copy(dest, Gio::FILE_COPY_NONE);
    }

    files.clear();
    const std::string old_backup_dir = Glib::build_filename(
                                         old_note_dir,
                                         "Backup");
    sharp::directory_get_files_with_ext(old_backup_dir,
                                        ".note", files);

    for (std::list<std::string>::const_iterator iter = files.begin();
         files.end() != iter; ++iter) {
      const Glib::RefPtr<Gio::File> src = Gio::File::create_for_path(
                                            *iter);
      const std::string dest_path
          = Glib::build_filename(m_backup_dir,
                                 Glib::path_get_basename(*iter));
      const Glib::RefPtr<Gio::File> dest = Gio::File::create_for_path(
                                                        dest_path);
      src->copy(dest, Gio::FILE_COPY_NONE);
    }
  }

  void NoteManager::on_exiting_event()
  {
    m_addin_mgr->shutdown_application_addins();

    DBG_OUT("Saving unsaved notes...");
      
    // Use a copy of the notes to prevent bug #510442 (crash on exit
    // when iterating the notes to save them.
    Note::List notesCopy(m_notes);
    for(Note::List::const_iterator iter = notesCopy.begin();
        iter != notesCopy.end(); ++iter) {
      const Note::Ptr & note(*iter);
      note->save();
    }
  }

  void NoteManager::delete_note(const Note::Ptr & note)
  {
    if (sharp::file_exists(note->file_path())) {
      if (!m_backup_dir.empty()) {
        if (!sharp::directory_exists(m_backup_dir)) {
          sharp::directory_create(m_backup_dir);
        }
        std::string backup_path 
          = Glib::build_filename(m_backup_dir, sharp::file_filename(note->file_path()));

        if (sharp::file_exists(backup_path))
          sharp::file_delete(backup_path);

        sharp::file_move(note->file_path(), backup_path);
      } 
      else {
        sharp::file_delete(note->file_path());
      }
    }

    m_notes.remove(note);
    note->delete_note();

    DBG_OUT("Deleting note '%s'.", note->get_title().c_str());

//    if (NoteDeleted != null)
    signal_note_deleted(note);
  }

  std::string NoteManager::make_new_file_name() const
  {
    return make_new_file_name (sharp::uuid().string());
  }

  std::string NoteManager::make_new_file_name(const std::string & guid) const
  {
    return Glib::build_filename(m_notes_dir, guid + ".note");
  }

  Note::Ptr NoteManager::create()
  {
    return create("");
  }

  std::string NoteManager::split_title_from_content(std::string title, std::string & body)
  {
    body = "";

    if (title.empty())
      return "";

    title = sharp::string_trim(title);
    if (title.empty())
      return "";

    std::vector<std::string> lines;
    sharp::string_split(lines, title, "\n\r");
    if (lines.size() > 0) {
      title = lines [0];
      title = sharp::string_trim(title);
      title = sharp::string_trim(title, ".,;");
      if (title.empty())
        return "";
    }

    if (lines.size() > 1)
      body = lines [1];

    return title;
  }


  Note::Ptr NoteManager::create (const std::string & title)
  {
    return create_new_note(title, "");
  }


  Note::Ptr NoteManager::create(const std::string & title, const std::string & xml_content)
  {
    return create_new_note(title, xml_content, "");
  }

  Note::Ptr NoteManager::import_note(const std::string & file_path)
  {
    std::string dest_file = Glib::build_filename(m_notes_dir, 
                                                 sharp::file_filename(file_path));
    
    if(sharp::file_exists(dest_file)) {
      dest_file = make_new_file_name();
    }
    Note::Ptr note;
    try {
      sharp::file_copy(file_path, dest_file);

      // TODO: make sure the title IS unique.
      note = Note::load(dest_file, *this);
      add_note(note);
    }
    catch(...)
    {
    }
    return note;
  }


  Note::Ptr NoteManager::create_with_guid (const std::string & title, const std::string & guid)
  {
    return create_new_note(title, guid);
  }

  // Create a new note with the specified title from the default
  // template note. Optionally the body can be overridden.
  Note::Ptr NoteManager::create_new_note (std::string title, const std::string & guid)
  {
    std::string body;

    title = split_title_from_content (title, body);
      
    if (title.empty()) {
      title = get_unique_name(_("New Note"));
    }

    Note::Ptr template_note = get_or_create_template_note();

    if (body.empty()) {
      return create_note_from_template(title, template_note, guid);
    }

    // Use a simple "Describe..." body and highlight
    // it so it can be easily overwritten
    std::string content = get_note_template_content(title);
    Note::Ptr new_note = create_new_note (title, content, guid);

    // Select the inital text so typing will overwrite the body text
    new_note->get_buffer()->select_note_body();

    return new_note;
  }

  // Create a new note with the specified Xml content
  Note::Ptr NoteManager::create_new_note(const std::string & title, const std::string & xml_content, 
                                        const std::string & guid)
  { 
    if (title.empty())
      throw sharp::Exception("Invalid title");

    if (find(title))
      throw sharp::Exception("A note with this title already exists: " + title);

    std::string filename;
    if (!guid.empty())
      filename = make_new_file_name (guid);
    else
      filename = make_new_file_name ();

    Note::Ptr new_note = Note::create_new_note (title, filename, *this);
    new_note->set_xml_content(xml_content);
    new_note->signal_renamed().connect(sigc::mem_fun(*this, &NoteManager::on_note_rename));
    new_note->signal_saved().connect(sigc::mem_fun(*this, &NoteManager::on_note_save));

    m_notes.push_back(new_note);

    // Load all the addins for the new note
    m_addin_mgr->load_addins_for_note (new_note);

    signal_note_added(new_note);

    return new_note;
  }

  Note::Ptr NoteManager::find_template_note() const
  {
    Note::Ptr template_note;
    Tag::Ptr template_tag = ITagManager::obj().get_system_tag(ITagManager::TEMPLATE_NOTE_SYSTEM_TAG);
    if(!template_tag) {
      return template_note;
    }
    std::list<Note*> notes;
    template_tag->get_notes(notes);
    for (std::list<Note*>::iterator iter = notes.begin(); iter != notes.end(); ++iter) {
      Note::Ptr note = (*iter)->shared_from_this();
      if (!notebooks::NotebookManager::obj().get_notebook_from_note(note)) {
        template_note = note;
        break;
      }
    }

    return template_note;
  }

  /// <summary>
  /// Get the existing template note or create a new one
  /// if it doesn't already exist.
  /// </summary>
  /// <returns>
  /// A <see cref="Note"/>
  /// </returns>
  Note::Ptr NoteManager::get_or_create_template_note()
  {
    Note::Ptr template_note = find_template_note();
    if (!template_note) {
      std::string title = m_default_note_template_title;
      if (find(title)) {
        title = get_unique_name(title);
      }
      template_note =
        create (title,
                get_note_template_content(title));
          
      // Select the initial text
      Glib::RefPtr<NoteBuffer> buffer = template_note->get_buffer();
      buffer->select_note_body();

      // Flag this as a template note
      Tag::Ptr template_tag = ITagManager::obj().get_or_create_system_tag(ITagManager::TEMPLATE_NOTE_SYSTEM_TAG);
      template_note->add_tag(template_tag);

      template_note->queue_save(CONTENT_CHANGED);
    }
      
    return template_note;
  }
    
  std::string NoteManager::get_note_template_content(const std::string & title)
  {
    return str(boost::format("<note-content>"
                             "<note-title>%1%</note-title>\n\n"
                             "%2%"
                             "</note-content>") 
               % utils::XmlEncoder::encode (title)
               % _("Describe your new note here."));
  }

  size_t NoteManager::trie_max_length()
  {
    return m_trie_controller->title_trie()->max_length();
  }


  TrieHit<Note::WeakPtr>::ListPtr NoteManager::find_trie_matches(const std::string & match)
  {
    return m_trie_controller->title_trie()->find_matches(match);
  }

  Note::Ptr NoteManager::find(const std::string & linked_title) const
  {
    for(Note::List::const_iterator iter = m_notes.begin();
        iter != m_notes.end(); ++iter) {
      const Note::Ptr & note(*iter);
      if (sharp::string_to_lower(note->get_title()) == sharp::string_to_lower(linked_title))
        return note;
    }
    return Note::Ptr();
  }

  Note::Ptr NoteManager::find_by_uri(const std::string & uri) const
  {
    for(Note::List::const_iterator iter = m_notes.begin();
        iter != m_notes.end(); ++iter) {
      const Note::Ptr & note(*iter);
      if (note->uri() == uri) {
        return note;
      }
    }
    return Note::Ptr();
  }

  std::string NoteManager::sanitize_xml_content(const std::string & xml_content)
  {
    std::string::size_type pos = xml_content.find('\n');
    int i = (pos == std::string::npos) ? -1 : pos;
    std::string result(xml_content);

    while (--i >= 0) {
      if(xml_content[i] == '\r') {
        continue;
      }

      if(std::isspace(result[i])) {
        result.erase(i, 1);
      }
      else {
        break;
      }
    }

    return result;
  }

  /// <summary>
  /// Creates a new note with the given titel based on the template note.
  /// </summary>
  /// <param name="title">
  /// A <see cref="System.String"/>
  /// </param>
  /// <param name="template_note">
  /// A <see cref="Note"/>
  /// </param>
  /// <returns>
  /// A <see cref="Note"/>
  /// </returns>
  Note::Ptr NoteManager::create_note_from_template(const std::string & title, const Note::Ptr & template_note)
  {
    return create_note_from_template(title, template_note, "");
  }

  // Creates a new note with the given title and guid with body based on
  // the template note.
  Note::Ptr NoteManager::create_note_from_template(const std::string & title, const Note::Ptr & template_note, const std::string & guid)
  {
    std::string new_title(title);
    Tag::Ptr template_save_title = ITagManager::obj().get_or_create_system_tag(ITagManager::TEMPLATE_NOTE_SAVE_TITLE_SYSTEM_TAG);
    if(template_note->contains_tag(template_save_title)) {
      new_title = get_unique_name(template_note->get_title());
    }

    // Use the body from the template note
    std::string xml_content = sharp::string_replace_first(template_note->xml_content(),
                                                          utils::XmlEncoder::encode(template_note->get_title()),
                                                          utils::XmlEncoder::encode(new_title));
    xml_content = sanitize_xml_content(xml_content);

    Note::Ptr new_note = create_new_note(new_title, xml_content, guid);

    // Copy template note's properties
    Tag::Ptr template_save_size = ITagManager::obj().get_or_create_system_tag(ITagManager::TEMPLATE_NOTE_SAVE_SIZE_SYSTEM_TAG);
    if(template_note->data().has_extent() && template_note->contains_tag(template_save_size)) {
      new_note->data().height() = template_note->data().height();
      new_note->data().width() = template_note->data().width();
    }

    Glib::RefPtr<Gtk::TextBuffer> buffer = new_note->get_buffer();
    Gtk::TextIter cursor, selection;
    Tag::Ptr template_save_selection = ITagManager::obj().get_or_create_system_tag(ITagManager::TEMPLATE_NOTE_SAVE_SELECTION_SYSTEM_TAG);
    if(template_note->contains_tag(template_save_selection)) {
      Glib::ustring template_title = template_note->get_title();
      int cursor_pos = template_note->data().cursor_position();
      int selection_bound = template_note->data().selection_bound_position();
      if(cursor_pos == 0) {
        // the created note has different title from template, take that into account
        // selection starts at the beginning of title
        // if it ends at the end of title, select entire title
        // if it ends after the title, keep the same offset from the end selected
        // otherwise select nothing, we don't know, what the selected part of title means anyway
        cursor = buffer->get_iter_at_offset(0);
        selection = cursor;
        if(selection_bound == int(template_title.size())) {
          selection.forward_to_line_end();
        }
        else if(selection_bound > int(template_title.size())) {
          selection.forward_to_line_end();
          selection.forward_chars(selection_bound - template_title.size());
        }
      }
      else if(cursor_pos <= int(template_title.size())) {
        cursor = buffer->get_iter_at_line(1);
        selection = cursor;
        selection.forward_chars(selection_bound - template_title.size() - 1); // skip title and new line
      }
      else {
        cursor = buffer->get_iter_at_offset(cursor_pos - template_title.size() + title.size() - 1);
        selection = buffer->get_iter_at_offset(selection_bound - template_title.size() + title.size() - 1);
      }
    }
    else {
      // move cursor to the start of first word after the title
      cursor = buffer->get_iter_at_line(1);
      while(!cursor.starts_word() && cursor.forward_char());
      selection = cursor;
    }

    buffer->place_cursor(cursor);
    if(selection != cursor) {
      buffer->move_mark(buffer->get_selection_bound(), selection);
    }

    return new_note;
  }

  // Find a title that does not exist using basename
  std::string NoteManager::get_unique_name(const std::string & basename) const
  {
    int id = 1;  // starting point
    std::string title;
    while (true) {
      title = str(boost::format("%1% %2%") % basename % id++);
      if (!find (title)) {
        break;
      }
    }

    return title;
  }

  Note::List NoteManager::get_notes_linking_to(const std::string & title) const
  {
    std::string tag = "<link:internal>" + utils::XmlEncoder::encode(title) + "</link:internal>";
    Note::List result;
    FOREACH(const Note::Ptr & note, m_notes) {
      if(note->get_title() != title) {
        if(note->get_complete_note_xml().find(tag) != std::string::npos) {
          result.push_back(note);
        }
      }
    }
    return result;
  }


  TrieController::TrieController (NoteManager & manager)
    : m_manager(manager)
    ,  m_title_trie(NULL)
  {
    m_manager.signal_note_deleted.connect(sigc::mem_fun(*this, &TrieController::on_note_deleted));
    m_manager.signal_note_added.connect(sigc::mem_fun(*this, &TrieController::on_note_added));
    m_manager.signal_note_renamed.connect(sigc::mem_fun(*this, &TrieController::on_note_renamed));

    update ();
  }

  TrieController::~TrieController()
  {
    delete m_title_trie;
  }

  void TrieController::on_note_added (const Note::Ptr & note)
  {
    add_note (note);
  }

  void TrieController::on_note_deleted (const Note::Ptr & )
  {
    update ();
  }

  void TrieController::on_note_renamed (const Note::Ptr & , const std::string & )
  {
    update ();
  }

  void TrieController::add_note(const Note::Ptr & note)
  {
    m_title_trie->add_keyword (note->get_title(), note);
    m_title_trie->compute_failure_graph();
  }

  void TrieController::update ()
  {
    if(m_title_trie) {
      delete m_title_trie;
    }
    m_title_trie = new TrieTree<Note::WeakPtr>(false /* !case_sensitive */);

    for(Note::List::const_iterator iter =  m_manager.get_notes().begin();
        iter !=  m_manager.get_notes().end(); ++iter) {
      const Note::Ptr & note(*iter);
      m_title_trie->add_keyword (note->get_title(), note);
    }
    m_title_trie->compute_failure_graph();
  }

}
