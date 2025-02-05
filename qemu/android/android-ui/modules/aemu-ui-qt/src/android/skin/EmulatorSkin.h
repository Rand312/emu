/* Copyright (C) 2022 The Android Open Source Project
 **
 ** This software is licensed under the terms of the GNU General Public
 ** License version 2, as published by the Free Software Foundation, and
 ** may be copied, distributed, and modified under those terms.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 */

#pragma once
#include <QPixmap>
#include <memory>

class EmulatorSkin final {
public:
    std::shared_ptr<QPixmap> getSkinPixmap();

    bool isPortrait() { return mSkinPixmapIsPortrait; };

    EmulatorSkin();

    void reset();

    static EmulatorSkin* getInstance();

private:
    std::shared_ptr<QPixmap> mRawSkinPixmap;  // For masking frameless AVDs
    bool mSkinPixmapIsPortrait{true};
};
