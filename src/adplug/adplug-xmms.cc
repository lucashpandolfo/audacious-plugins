/*
   AdPlug/XMMS - AdPlug XMMS Plugin
   Copyright (C) 2002, 2003 Simon Peter <dn.tlp@gmx.net>

   AdPlug/XMMS is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This plugin is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this plugin; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "config.h"

#include <algorithm>
#include <pthread.h>
#include <sstream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "adplug.h"
#include "emuopl.h"
#include "silentopl.h"
#include "players.h"

extern "C" {
#include <audacious/misc.h>
#include <libaudcore/audstrings.h>

#include "adplug-xmms.h"
}

/***** Defines *****/

// Version string
#define ADPLUG_NAME	"AdPlug (AdLib Sound Player)"

// Sound buffer size in samples
#define SNDBUFSIZE	512

// AdPlug's 8 and 16 bit audio formats
#define FORMAT_8	FMT_U8
#define FORMAT_16	FMT_S16_NE

// Default file name of AdPlug's database file
#define ADPLUGDB_FILE		"adplug.db"

// Default AdPlug user's configuration subdirectory
#define ADPLUG_CONFDIR		".adplug"

/***** Global variables *****/

static bool_t audio_error = FALSE;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool_t stop_flag;

// Configuration (and defaults)
static struct
{
  int freq;
  bool_t bit16, stereo, endless;
  CPlayers players;
} conf =
{
44100l, true, false, false, CAdPlug::getPlayers()};

// Player variables
static struct
{
  CPlayer *p;
  CAdPlugDatabase *db;
  unsigned int subsong, songlength;
  int seek;
  char * filename;
} plr = {0, 0, 0, 0, -1, NULL};

static InputPlayback *playback;

/***** Debugging *****/

#ifdef DEBUG

#include <stdarg.h>

static void
dbg_printf (const char *fmt, ...)
{
  va_list argptr;

  va_start (argptr, fmt);
  vfprintf (stderr, fmt, argptr);
  va_end (argptr);
}

#else

static void
dbg_printf (const char *fmt, ...)
{
}

#endif

static CPlayer *
factory (VFSFile * fd, Copl * newopl)
{
  return CAdPlug::factory (fd, newopl, conf.players);
}

extern "C" {
void adplug_stop(InputPlayback * data);
bool_t adplug_play(InputPlayback * data, const char * filename, VFSFile * file, int start_time, int stop_time, bool_t pause);
}

/***** Main player (!! threaded !!) *****/

extern "C" Tuple * adplug_get_tuple (const char * filename, VFSFile * fd)
{
  Tuple * ti = NULL;
  CSilentopl tmpopl;

  if (!fd)
    return NULL;

  CPlayer *p = factory (fd, &tmpopl);

  if (p)
  {
    ti = tuple_new_from_filename (filename);

    if (! p->getauthor().empty())
      tuple_set_str(ti, FIELD_ARTIST, NULL, p->getauthor().c_str());

    if (! p->gettitle().empty())
      tuple_set_str(ti, FIELD_TITLE, NULL, p->gettitle().c_str());
    else if (! p->getdesc().empty())
      tuple_set_str(ti, FIELD_TITLE, NULL, p->getdesc().c_str());

    tuple_set_str(ti, FIELD_CODEC, NULL, p->gettype().c_str());
    tuple_set_str(ti, FIELD_QUALITY, NULL, "sequenced");
    tuple_set_int(ti, FIELD_LENGTH, NULL, p->songlength (plr.subsong));
    delete p;
  }

  return ti;
}

// Define sampsize macro (only usable inside play_loop()!)
#define sampsize ((bit16 ? 2 : 1) * (stereo ? 2 : 1))

static bool_t play_loop (InputPlayback * playback, const char * filename,
 VFSFile * fd)
/* Main playback thread. Takes the filename to play as argument. */
{
  dbg_printf ("play_loop(\"%s\"): ", filename);
  CEmuopl opl (conf.freq, conf.bit16, conf.stereo);
  long toadd = 0, i, towrite;
  char *sndbuf, *sndbufpos;
  bool playing = true,          // Song self-end indicator.
    bit16 = conf.bit16,          // Duplicate config, so it doesn't affect us if
    stereo = conf.stereo;        // the user changes it while we're playing.
  unsigned long freq = conf.freq;

  if (!fd)
    return FALSE;

  // Try to load module
  dbg_printf ("factory, ");
  if (!(plr.p = factory (fd, &opl)))
  {
    dbg_printf ("error!\n");
    // MessageBox("AdPlug :: Error", "File could not be opened!", "Ok");
    return FALSE;
  }

  // reset to first subsong on new file
  dbg_printf ("subsong, ");
  if (! plr.filename || strcmp (filename, plr.filename))
  {
    free (plr.filename);
    plr.filename = strdup (filename);
    plr.subsong = 0;
  }

  // Allocate audio buffer
  dbg_printf ("buffer, ");
  sndbuf = (char *) malloc (SNDBUFSIZE * sampsize);

  // Set XMMS main window information
  dbg_printf ("xmms, ");
  playback->set_params (playback, freq * sampsize * 8, freq, stereo ? 2 : 1);

  // Rewind player to right subsong
  dbg_printf ("rewind, ");
  plr.p->rewind (plr.subsong);

  pthread_mutex_lock (& mutex);
  plr.seek = -1;
  stop_flag = FALSE;
  playback->set_pb_ready (playback);
  pthread_mutex_unlock (& mutex);

  // main playback loop
  dbg_printf ("loop.\n");
  while ((playing || conf.endless))
  {
    pthread_mutex_lock (& mutex);

    if (stop_flag)
    {
        pthread_mutex_unlock (& mutex);
        break;
    }

    // seek requested ?
    if (plr.seek != -1)
    {
      int time = playback->output->written_time ();

      // backward seek ?
      if (plr.seek < time)
      {
        plr.p->rewind (plr.subsong);
        time = 0;
      }

      // seek to requested position
      while (time < plr.seek && plr.p->update ())
        time += (int) (1000 / plr.p->getrefresh ());

      // Reset output plugin and some values
      playback->output->flush (time);
      plr.seek = -1;
    }

    pthread_mutex_unlock (& mutex);

    // fill sound buffer
    towrite = SNDBUFSIZE;
    sndbufpos = sndbuf;
    while (towrite > 0)
    {
      while (toadd < 0)
      {
        toadd += freq;
        playing = plr.p->update ();
      }
      i = MIN (towrite, (long) (toadd / plr.p->getrefresh () + 4) & ~3);
      opl.update ((short *) sndbufpos, i);
      sndbufpos += i * sampsize;
      towrite -= i;
      toadd -= (long) (plr.p->getrefresh () * i);
    }

    playback->output->write_audio (sndbuf, SNDBUFSIZE * sampsize);
  }

  pthread_mutex_lock (& mutex);
  stop_flag = FALSE;
  pthread_mutex_unlock (& mutex);

  // free everything and exit
  dbg_printf ("free");
  delete plr.p;
  plr.p = 0;
  free (sndbuf);
  dbg_printf (".\n");
  return TRUE;
}

// sampsize macro not useful anymore.
#undef sampsize

/***** Informational *****/

extern "C" int
adplug_is_our_fd (const char * filename, VFSFile * fd)
{
  CSilentopl tmpopl;

  CPlayer *p = factory (fd, &tmpopl);

  dbg_printf ("adplug_is_our_file(\"%s\"): returned ", filename);

  if (p)
  {
    delete p;
    dbg_printf ("TRUE\n");
    return TRUE;
  }

  dbg_printf ("FALSE\n");
  return FALSE;
}

/***** Player control *****/

extern "C" bool_t
adplug_play (InputPlayback * data, const char * filename, VFSFile * file, int start_time, int stop_time, bool_t pause)
{
  playback = data;
  dbg_printf ("adplug_play(\"%s\"): ", filename);
  audio_error = FALSE;

  // open output plugin
  dbg_printf ("open, ");
  if (!playback->output->
      open_audio (conf.bit16 ? FORMAT_16 : FORMAT_8, conf.freq,
                  conf.stereo ? 2 : 1))
  {
    audio_error = TRUE;
    return TRUE;
  }

  play_loop (playback, filename, file);
  return FALSE;
}

extern "C" void adplug_stop (InputPlayback * p)
{
    pthread_mutex_lock (& mutex);

    if (! stop_flag)
    {
        stop_flag = TRUE;
        p->output->abort_write ();
    }

    pthread_mutex_unlock (& mutex);
}

extern "C" void adplug_pause (InputPlayback * p, bool_t pause)
{
    pthread_mutex_lock (& mutex);

    if (! stop_flag)
        p->output->pause (pause);

    pthread_mutex_unlock (& mutex);
}

extern "C" void adplug_mseek (InputPlayback * p, int time)
{
    pthread_mutex_lock (& mutex);

    if (! stop_flag)
    {
        plr.seek = time;
        p->output->abort_write();
    }

    pthread_mutex_unlock (& mutex);
}

/***** Configuration file handling *****/

#define CFG_VERSION "AdPlug"

static const char * const adplug_defaults[] = {
 "16bit", "TRUE",
 "Stereo", "FALSE",
 "Frequency", "44100",
 "Endless", "FALSE",
 NULL};

extern "C" bool_t adplug_init (void)
{
  aud_config_set_defaults (CFG_VERSION, adplug_defaults);

  conf.bit16 = aud_get_bool (CFG_VERSION, "16bit");
  conf.stereo = aud_get_bool (CFG_VERSION, "Stereo");
  conf.freq = aud_get_int (CFG_VERSION, "Frequency");
  conf.endless = aud_get_bool (CFG_VERSION, "Endless");

  // Read file type exclusion list
  dbg_printf ("exclusion, ");
  {
    char * cfgstr = aud_get_string (CFG_VERSION, "Exclude");

    if (cfgstr[0])
    {
        char exclude[strlen (cfgstr) + 2];
        memcpy (exclude, cfgstr, sizeof exclude - 1);
        exclude[sizeof exclude - 1] = 0;
        string_replace_char (exclude, ':', 0);

        for (char * p = exclude; * p; p += strlen (p) + 1)
            conf.players.remove (conf.players.lookup_filetype (p));
    }

    free (cfgstr);
  }

  // Load database from disk and hand it to AdPlug
  dbg_printf ("database");
  plr.db = new CAdPlugDatabase;

  {
    const char *homedir = getenv ("HOME");

    if (homedir)
    {
      std::string userdb;
      userdb = std::string ("file://") + homedir + "/" ADPLUG_CONFDIR "/" + ADPLUGDB_FILE;

      if (vfs_file_test (userdb.c_str (), VFS_EXISTS))
      {
        plr.db->load (userdb);    // load user's database
        dbg_printf (" (userdb=\"%s\")", userdb.c_str());
      }
    }
  }
  CAdPlug::set_database (plr.db);
  dbg_printf (".\n");

  return TRUE;
}

extern "C" void
adplug_quit (void)
{
  // Close database
  dbg_printf ("db, ");
  if (plr.db)
    delete plr.db;

  free (plr.filename);
  plr.filename = NULL;

  aud_set_bool (CFG_VERSION, "16bit", conf.bit16);
  aud_set_bool (CFG_VERSION, "Stereo", conf.stereo);
  aud_set_int (CFG_VERSION, "Frequency", conf.freq);
  aud_set_bool (CFG_VERSION, "Endless", conf.endless);

  dbg_printf ("exclude, ");
  std::string exclude;
  for (CPlayers::const_iterator i = CAdPlug::getPlayers().begin ();
       i != CAdPlug::getPlayers().end (); i++)
    if (find (conf.players.begin (), conf.players.end (), *i) ==
        conf.players.end ())
    {
      if (!exclude.empty ())
        exclude += ":";
      exclude += (*i)->filetype;
    }

  aud_set_string (CFG_VERSION, "Exclude", exclude.c_str ());
}
