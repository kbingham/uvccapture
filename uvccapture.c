
/*******************************************************************************
#             uvccapture: USB UVC Video Class Snapshot Software                #
#This package work with the Logitech UVC based webcams with the mjpeg feature  #
#All the decoding is in user space with the embedded jpeg decoder              #
#.                                                                             #
# 	Orginally Copyright (C) 2005 2006 Laurent Pinchart &&  Michel Xhaard   #
#       Modifications Copyright (C) 2006  Gabriel A. Devenyi                   #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev.h>
#include "v4l2uvc.h"

static const char version[] = VERSION;

void
usage (void)
{
  fprintf (stderr, "uvccapture version %s\n", version);
  fprintf (stderr, "Usage is uvccapture [options]\n");
  fprintf (stderr, "Options:\n");
  fprintf (stderr, "-v\t\tVerbose\n");
  fprintf (stderr, "-o<filename>\tOutput filename\n");
  fprintf (stderr, "-d<device>\tV4L2 Device\n");
  fprintf (stderr, "-x<width>\tImage Width(must be supported by device)\n");
  fprintf (stderr, "-y<height>\tImage Height(must be supported by device)\n");
  fprintf (stderr, "-r\t\tUse read instead of mmap for image capture\n");
  fprintf (stderr,
	   "-t<integer>\tTake continuous shots with <integer> seconds between them (0 for single shot)\n");
  fprintf (stderr, "Image Properties:\n");
  fprintf (stderr, "-B<integer>\tBrightness\n");
  fprintf (stderr, "-C<integer>\tContrast\n");
  fprintf (stderr, "-S<integer>\tSaturation\n");
  fprintf (stderr, "-G<integer>\tGain\n");
  exit (8);
}

int
main (int argc, char *argv[])
{
  char *videodevice = "/dev/video0";
  char *outputfile = "snap.jpg";
  int format = V4L2_PIX_FMT_MJPEG;
  int grabmethod = 1;
  int width = 320;
  int height = 240;
  int brightness = 0, contrast = 0, saturation = 0, gain = 0;
  int verbose = 0;
  int delay = 0;
  time_t ref_time;
  struct vdIn *videoIn;
  FILE *file;

  //Options Parsing (FIXME)
  while ((argc > 1) && (argv[1][0] == '-')) {
    switch (argv[1][1]) {
    case 'v':
      verbose = 1;
      break;

    case 'o':
      outputfile = &argv[1][2];
      break;

    case 'd':
      videodevice = &argv[1][2];
      break;

    case 'x':
      width = atoi (&argv[1][2]);
      break;

    case 'y':
      height = atoi (&argv[1][2]);
      break;

    case 'r':
      grabmethod = 0;
      break;

    case 't':
      delay = atoi (&argv[1][2]);
      break;

    case 'B':
      brightness = atoi (&argv[1][2]);
      break;

    case 'C':
      contrast = atoi (&argv[1][2]);
      break;

    case 'S':
      saturation = atoi (&argv[1][2]);
      break;

    case 'G':
      gain = atoi (&argv[1][2]);
      break;

    case 'h':
      usage ();
      break;

    default:
      fprintf (stderr, "Unknown option %s \n", argv[1]);
      usage ();
    }
    ++argv;
    --argc;
  }

  if (verbose == 1) {
    fprintf (stderr, "Using videodevice: %s\n", videodevice);
    fprintf (stderr, "Saving images to: %s\n", outputfile);
    fprintf (stderr, "Image size: %dx%d\n", width, height);
    fprintf (stderr, "Taking snapshot every %d seconds\n", delay);
    if (grabmethod == 1)
      fprintf (stderr, "Taking images using mmap\n");
    else
      fprintf (stderr, "Takking images using read\n");
  }
  videoIn = (struct vdIn *) calloc (1, sizeof (struct vdIn));
  if (init_videoIn
      (videoIn, (char *) videodevice, width, height, format, grabmethod) < 0)
    exit (1);

  //Reset all camera controls
  if (verbose == 1)
    fprintf (stderr, "Resetting camera settings\n");
  v4l2ResetControl (videoIn, V4L2_CID_BRIGHTNESS);
  v4l2ResetControl (videoIn, V4L2_CID_CONTRAST);
  v4l2ResetControl (videoIn, V4L2_CID_SATURATION);
  v4l2ResetControl (videoIn, V4L2_CID_GAIN);

  //Setup Camera Parameters
  if (brightness != 0) {
    if (verbose == 1)
      fprintf (stderr, "Setting camera brightness to %d\n", brightness);
    v4l2SetControl (videoIn, V4L2_CID_BRIGHTNESS, brightness);
  }
  if (contrast != 0) {
    if (verbose == 1)
      fprintf (stderr, "Setting camera contrast to %d\n", contrast);
    v4l2SetControl (videoIn, V4L2_CID_CONTRAST, contrast);
  }
  if (saturation != 0) {
    if (verbose == 1)
      fprintf (stderr, "Setting camera saturation to %d\n", saturation);
    v4l2SetControl (videoIn, V4L2_CID_SATURATION, saturation);
  }
  if (gain != 0) {
    if (verbose == 1)
      fprintf (stderr, "Setting camera gain to %d\n", gain);
    v4l2SetControl (videoIn, V4L2_CID_GAIN, gain);
  }
  ref_time = time (NULL);

  while (1) {
    if (uvcGrab (videoIn) < 0) {
      fprintf (stderr, "Error grabbing\n");
      exit (1);
    }

    if (difftime (time (NULL), ref_time) > delay) {
      if (verbose == 1)
	fprintf (stderr, "Saving image to: %s\n", outputfile);
      file = fopen (outputfile, "wb");
      if (file != NULL) {
	fwrite (videoIn->tmpbuffer, videoIn->buf.bytesused + DHT_SIZE, 1,
		file);
	fclose (file);
	videoIn->getPict = 0;
      }
      ref_time = time (NULL);
    }
    if (delay == 0)
      break;
  }
  close_v4l2 (videoIn);
  free (videoIn);

  return 0;
}
