// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw.h"

#include "esp_log.h"
#include "esp_partition.h"

namespace tracks {

static const char* TAG = "tracks";

bool begin(nano::Library& lib)
{
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "tracks");
    if (!p) { ESP_LOGE(TAG, "no `tracks` partition (partitions.csv)"); return false; }
    const void* base = nullptr;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &base, &h) != ESP_OK) {
        ESP_LOGE(TAG, "mmap of %u KB failed", (unsigned)(p->size / 1024));
        return false;
    }
    if (!lib.open(base, p->size)) {
        ESP_LOGW(TAG, "no track pack at 0x%x: run tools/pack_tracks.py and flash it there", (unsigned)p->address);
        return false;
    }
    ESP_LOGI(TAG, "%d tracks in %u KB at 0x%x", lib.count(), (unsigned)(p->size / 1024), (unsigned)p->address);
    for (int i = 0; i < lib.count(); i++) {
        nano::Track t;
        if (lib.get(i, t))
            ESP_LOGI(TAG, "  %02d %s: %u Hz %s, %u s", i + 1, t.name, (unsigned)t.pcm.rate,
                     t.pcm.channels == 2 ? "stereo" : "mono", (unsigned)(t.pcm.frames / t.pcm.rate));
    }
    return true;
}

} // namespace tracks
