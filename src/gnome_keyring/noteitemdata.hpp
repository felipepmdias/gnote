/*
 * gnote
 *
 * Copyright (C) 2012 Aurimas Cernius
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


#ifndef _GNOME_KEYRING_NOTEITEMDATA_HPP_
#define _GNOME_KEYRING_NOTEITEMDATA_HPP_

#include "itemdata.hpp"


namespace gnome {
namespace keyring {

class NoteItemData
  : public ItemData
{
public:
  static ItemData::Ptr create()
    {
      return ItemData::Ptr(new NoteItemData);
    }

  virtual ItemType type() const
    {
      return NOTE;
    }
};

}
}

#endif