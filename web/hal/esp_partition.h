#pragma once
// esp_partition stubs for GrvEP simulator — all operations return NULL/error

#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
#define ESP_OK    0
#define ESP_FAIL  -1

#define ESP_PARTITION_TYPE_APP  0x00
#define ESP_PARTITION_TYPE_DATA 0x01
#define ESP_PARTITION_SUBTYPE_DATA_UNDEFINED 0xFF
#define ESP_PARTITION_SUBTYPE_ANY 0xFF

typedef int esp_partition_mmap_memory_t;
#define SPI_FLASH_MMAP_DATA   0
#define ESP_PARTITION_MMAP_DATA 0

typedef struct esp_partition_t {
    int type;
    int subtype;
    size_t size;
    uint32_t address;
    char label[16];
} esp_partition_t;

typedef int esp_partition_mmap_handle_t;

static inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    return nullptr;
}
static inline esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t) {
    return ESP_FAIL;
}
static inline esp_err_t esp_partition_write(const esp_partition_t*, size_t, const void*, size_t) {
    return ESP_FAIL;
}
static inline esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t, size_t) {
    return ESP_FAIL;
}
static inline esp_err_t esp_partition_mmap(const esp_partition_t*, size_t, size_t,
    int, const void**, esp_partition_mmap_handle_t*) {
    return ESP_FAIL;
}
static inline void esp_partition_munmap(esp_partition_mmap_handle_t) {}

// esp_heap_caps
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 0)
#define MALLOC_CAP_8BIT     (1 << 2)
