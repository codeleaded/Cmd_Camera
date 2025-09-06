#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <jpeglib.h>

#define DEVICE "/dev/video0"  // Das Kamera-Gerät
#define WIDTH 640             // Bildbreite
#define HEIGHT 480            // Bildhöhe
#define NUM_IMAGES 10          // Anzahl der Bilder, die aufgenommen werden sollen

// Struktur für den Puffer
struct buffer {
    void *start;
    size_t length;
};

// Struktur für die Kamera
struct camera {
    int fd;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    struct buffer *buffers;
    int num_buffers;
};

// Funktion zum Dekodieren von JPEG-Daten in ARGB
unsigned int* decode_jpeg_to_argb(unsigned char* jpeg_data, size_t jpeg_size, int* width, int* height) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY buffer;
    
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    *width = cinfo.output_width;
    *height = cinfo.output_height;
    int row_stride = cinfo.output_width * cinfo.output_components;
    unsigned int* argb_buffer = malloc(cinfo.output_width * cinfo.output_height * sizeof(unsigned int));
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);

    for (int y = 0; y < cinfo.output_height; y++) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        for (int x = 0; x < cinfo.output_width; x++) {
            unsigned char r = buffer[0][x * 3 + 0];
            unsigned char g = buffer[0][x * 3 + 1];
            unsigned char b = buffer[0][x * 3 + 2];
            argb_buffer[y * cinfo.output_width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return argb_buffer;
}

// Funktion zum Speichern von JPEG-Daten
void save_jpeg(unsigned char* jpeg_data, size_t jpeg_size, const char* filename) {
    FILE* out_file = fopen(filename, "wb");
    if (out_file) {
        fwrite(jpeg_data, jpeg_size, 1, out_file);
        fclose(out_file);
        printf("Bild gespeichert als %s\n", filename);
    } else {
        perror("Fehler beim Öffnen der Datei");
    }
}

// Funktion zum Initialisieren der Kamera
int init_camera(struct camera* cam) {
    cam->fd = open(DEVICE, O_RDWR);
    if (cam->fd == -1) {
        perror("Fehler beim Öffnen des Geräts");
        return -1;
    }

    struct v4l2_capability cap;
    if (ioctl(cam->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("Fehler bei VIDIOC_QUERYCAP");
        return -1;
    }

    // Format für die Kamera festlegen
    memset(&cam->fmt, 0, sizeof(cam->fmt));
    cam->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cam->fmt.fmt.pix.width = WIDTH;
    cam->fmt.fmt.pix.height = HEIGHT;
    cam->fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;  // JPEG-Format
    cam->fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(cam->fd, VIDIOC_S_FMT, &cam->fmt) == -1) {
        perror("Fehler bei VIDIOC_S_FMT");
        return -1;
    }

    // Puffer anfordern
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = 4;  // Anzahl der Puffer
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam->fd, VIDIOC_REQBUFS, &reqbuf) == -1) {
        perror("Fehler bei VIDIOC_REQBUFS");
        return -1;
    }

    // Puffer für die Kamera zuweisen
    cam->buffers = malloc(reqbuf.count * sizeof(struct buffer));
    if (!cam->buffers) {
        perror("Fehler bei der Pufferzuweisung");
        return -1;
    }

    for (int i = 0; i < reqbuf.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("Fehler bei VIDIOC_QUERYBUF");
            return -1;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[i].start == MAP_FAILED) {
            perror("Fehler bei mmap");
            return -1;
        }

        // Debugging: Bestätigung der Pufferzuweisung
        printf("Puffer %d: Start: %p, Länge: %zu\n", i, cam->buffers[i].start, cam->buffers[i].length);

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) {
            perror("Fehler bei VIDIOC_QBUF");
            return -1;
        }
    }

    // Streaming aktivieren
    if (ioctl(cam->fd, VIDIOC_STREAMON, &reqbuf.type) == -1) {
        perror("Fehler bei VIDIOC_STREAMON");
        return -1;
    }

    return 0;
}

// Funktion zum Abrufen eines Bildes von der Kamera
int capture_image(struct camera* cam, unsigned char** jpeg_data, size_t* jpeg_size) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // Warten auf ein Bild, falls notwendig
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    struct timeval tv = { 2, 0 };  // Timeout von 2 Sekunden
    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        perror("Fehler bei select");
        return -1;
    } else if (r == 0) {
        fprintf(stderr, "Timeout beim Warten auf ein Bild\n");
        return -1;
    }

    // Bild abholen
    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == -1) {
        perror("Fehler bei VIDIOC_DQBUF");
        return -1;
    }

    // Bilddaten bereitstellen
    *jpeg_data = cam->buffers[buf.index].start;
    *jpeg_size = buf.bytesused;

    // Debugging: Anzeige der Bildgröße und des Index
    printf("Bildgröße: %zu Bytes, Index: %d\n", *jpeg_size, buf.index);

    // Puffer zurück in die Warteschlange einfügen
    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) {
        perror("Fehler bei VIDIOC_QBUF");
        return -1;
    }

    return 0;
}

// Funktion zum Freigeben der Kamera
void close_camera(struct camera* cam) {
    for (int i = 0; i < 4; i++) {
        munmap(cam->buffers[i].start, cam->buffers[i].length);
    }
    free(cam->buffers);
    close(cam->fd);
}

int main() {
    struct camera cam;

    // Kamera initialisieren
    if (init_camera(&cam) == -1) {
        return 1;
    }

    // Bild aufnehmen und speichern
    unsigned char* jpeg_data = NULL;
    size_t jpeg_size = 0;
    for (int i = 0; i < NUM_IMAGES; i++) {
        printf("Aufnahme Bild %d...\n", i + 1);

        if (capture_image(&cam, &jpeg_data, &jpeg_size) == -1) {
            fprintf(stderr, "Fehler beim Abrufen des Bildes %d\n", i + 1);
            continue;
        }

        // Bild speichern
        char filename[128];
        snprintf(filename, sizeof(filename), "image_%d.jpg", i + 1);
        save_jpeg(jpeg_data, jpeg_size, filename);
        usleep(500000);  // Warten für 500ms zwischen den Aufnahmen
    }

    // Kamera freigeben
    close_camera(&cam);

    return 0;
}
