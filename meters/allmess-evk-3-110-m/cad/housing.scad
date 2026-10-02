// flowtick-meter case for the Allmess EVK 3/110 +m water meter: a printed
// replacement for the blank module ("Leermodul") of the +m module bay that
// carries the TCRT5000L over the scanning disc and the ESP32-C6 Super Mini in
// front of it.
//
// Two parts, both printable without support:
//   adapter  - the blank-module shape with the pocket over the scanning disc
//              and a slot for the sensor. Print front face down.
//   cap      - the compartment for the Super Mini. Print lid down.
// They are joined with two M2 self-tapping screws.
//
// Coordinates, looking at the meter from the front:
//   x  to the right, y up, origin in the centre of the round meter head
//   z  towards you; z = 0 is the back rim that sits against the meter
//
// Measured on a removed blank module unless marked "guess"; the outline and
// the snap tongues come from a 300 dpi flatbed scan (0.085 mm/px), see
// ../photos/ and ../README.md.
//
// Copyright 2026 Jonas Brüstel
// SPDX-License-Identifier: Apache-2.0

part = "both";   // "adapter", "cap", "both" (preview, assembled), "print" (both, laid out for printing),
                 // "slot-test" (three sensor slots to find the press fit before printing the adapter),
                 // "open" (preview without the cap, to see board and sensor),
                 // "snap-test" (the rim with the snap tongues, to try the fit on the meter)

$fn = 96;

/* [Blank module] */
R            = 63.5 / 2;  // outer radius
D0           = 23.5;      // depth of the original blank module, back rim to front face
// Ours is deeper: the front plate moves out by lift, and in the room gained
// behind it sits the sleeve that holds the sensor -- ending where the floor of
// the original was, under which the window sits. Rim, tongues and inner wall stay
// where they were relative to the meter.
lift         = 6.0;
D            = D0 + lift;
wall         = 1.2;       // original is 1.0 (scan); a little more for PETG
front        = 1.6;       // front plate
edge_y       = 2.5;       // the straight edge sits this far ABOVE the centre -- more than a half disc (scan)
// The nose under the pocket, filling the step in the name plate (scan and
// calipers agree): a chamfer from x0 on the straight edge up to x1, then flat
// at nose_h above the edge until it meets the round.
nose_x0      = 3.3;
nose_x1      = 7.4;
nose_h       = 5.8;
// Only the round runs the full depth and sits on the meter. The other walls
// hang from the front and stop short (side-view scan):
straight_h   = 14.5 + lift;  // the inner straight wall, from the front face; ends 9 mm above the rim as on the original
inner_wall_on  = false;   // off: the rim along the edge is enough and the wall gets in the way when fitting
inner_wall_gap = 1.0;     // gap between the rim along the edge and the inner wall (the original has ~3 mm)
nose_depth   = 4.0 + lift;  // the nose is only a lip at the front (original ~3.5), grown with lift
edge_lip     = 4.0 + lift;  // the rim along the straight edge, the chamfer and the nose, front plate
                            // included: 4 mm, plus lift so it ends where it did before
// Three ribs inside at the bottom of the round, 22 mm high from the front,
// thinning towards the rim; 3 mm apart, 1.9 mm thick at the base.
ribs_on      = false;     // off for the first print; switch on if the adapter wobbles on the register
ribs_x       = [-4.9, 0, 4.9];
rib          = [1.9, 1.0, 2.0, 22];  // base thickness, tip thickness, radial depth, height

/* [Pocket over the scanning disc] */
pocket_d     = 25;        // inner diameter
pocket_wall_h = 18;       // the pocket is an open cup under the front plate; wall height from its inside.
                          // The window of the scanning disc reaches into it from behind.
// Off by default: on the original it shields the PM module's optics and centres
// the module on the window. Here the black housing keeps the light out and the
// snap tongues locate it -- and with 0.2 mm of play a slightly misplaced cup
// would sit on the window and keep the adapter from seating.
pocket_wall  = false;
pocket_clear = 0.4;       // added to the diameter
// Centre of the pocket = centre of the scanning disc, from a circle fit to
// the (blurred) pocket wall in the rim-side scan, 70 points, rms 0.27 mm; the
// fitted radius 12.9 mm at the wall centre gives the measured inner 25 mm.
// It sits close under the straight edge, which is why the window pokes up
// into the step of the name plate and why the module has its nose.
pocket_c     = [17.83, -2.79];

/* [Sensor: Vishay TCRT5000L] */
// Vishay datasheet 83760: 10.2 +0.1/-0.2 x 5.8 +0.1/-0.2, 7.0 +-0.2 to the
// top of the lens domes. Full cross-section only over the 3 mm next to the
// leads, tapering towards the lenses; two snap clips stand 3 mm off the lead
// side. Peak output at 2.5 mm, about 75 % at 5 mm.
tcrt         = [10.2, 5.8, 7.0];  // body, lenses on the -z face
// Undersize per side. Small on purpose: the part varies by 0.3 mm and FDM holes
// come out 0.1-0.2 mm small anyway. Find the value with part = "slot-test".
press        = 0.05;
sensor_dir   = -90;       // direction from the disc centre, degrees (0 = +x): straight down, the only
                          // side with room -- the round is close on the right, the board on the left
// The disc is about 18 mm across (meter photo), the red hub about 3 mm; the
// sensing spot, between the two lenses, should sit well inside the chrome /
// black sectors and clear of the hub.
sensor_off   = 4.5;       // from the disc centre along sensor_dir, lens axis radial, so both lenses look at the
                          // rotating sectors and the board fits beside the sensor, not above it
tilt         = 12;        // about the lens axis: moves the direct reflection off the window away from the phototransistor

/* [Compartment for the Super Mini] */
// Lying across, USB-C to the left (the free side; there is a wall on the
// right), the 5V pin row towards the straight edge, beside the sensor rather
// than above it. Held in the cap, parts side towards the lid: pushed in past
// small lips on four corner brackets, it rests against ribs hanging from the
// lid. The cap and the board come off the adapter as one.
board        = [26.04, 18.0, 1.0];  // length (x), width (y), thickness -- vendor drawing
board_pos    = [-21.0, -18.8];      // lower left corner; the USB-C end is at the left; clear of the straight wall
usb_over     = 1.3;       // how far the receptacle sticks out past the board edge
board_stand  = 5.3;       // lid to the top of the board: room for the USB-C plug overmould above the receptacle
bracket      = [3.5, 1.0];      // corner brackets hanging from the lid: leg length, wall
lip          = [0.3, 1.2];      // retaining lip at the bracket tips: reach under the board, ramp height (was 0.4 / 0.8, too stiff)
fit          = 0.2;       // play around the board in the brackets
cap_inner    = 10.0;      // board stand-off + board + the plug overmould reaching below it
lid          = 1.6;
cap_wall     = 1.6;
usb_notch    = [13, 10.0];  // width, height of the opening for the plug overmould

// Holes in the lid, positions from the vendor drawing (from the USB end /
// from the 5V edge of the board).
function on_board(from_usb, from_5v) = [board_pos.x + from_usb, board_pos.y + board.y - from_5v];
boot_pos     = on_board(9.3, 12.6);  // BOOT: hold to erase the WiFi settings
rst_pos      = on_board(9.1, 4.6);   // RST
button_d     = 3.0;

// The status LED needs no hole: it shines through the lid.

// Pin rows: 10 pads at 2.54 mm, centred on the board. 5V row from the USB end:
// 5V GND 3V3 20 19 18 15 14 9 8; other row: TX(16) RX(17) 0 1 2 3 4 5 6 7.
function pin_x(i) = (board.x - 9 * 2.54) / 2 + (i - 1) * 2.54;
wire_gap = 1.5;           // free on either side of a pad with a wire on it
// Stand-off ribs as [5V row?, from, to] along the board, from the USB end.
// 5V row: under 20 ... 8, leaving 5V/GND/3V3 free. Other row: under 16/17/0
// (from behind the status LED at the USB end) and 2 ... 7, a gap at GPIO1.
ribs = [
  [true,  pin_x(3) + wire_gap, board.x - 0.3],
  [false, 2.6,                 pin_x(4) - wire_gap],
  [false, pin_x(4) + wire_gap, board.x - 0.3],
];

/* [Snap tongues] */
// Where the original snapped onto the meter head. Both were broken off on the
// removed module, so the slots and tongues come from the rim scan and the
// catch on the tongue is a guess to be settled with part = "snap-test".
// [slot from, slot to, tongue from, tongue to] along y, on the wall at the
// given side; the tongue is free at the rim and joined at slot_depth.
snap_right   = [2.42, 6.82, 3.18, 6.14];   // in the end wall of the nose
// Test fits: first 3 mm towards the straight edge, then 2 back -- net 1 mm
// (scan: -9.01, -4.95, -8.25, -5.37).
snap_left    = [-9.01, -4.95, -8.25, -5.37] + [1, 1, 1, 1];
// The original slots are only ~4 mm deep (photos: 3.8-4.1), and both tongues
// broke. A 4 mm PETG tongue bends only ~0.2 mm before it fails; at 8 mm about
// 0.9 mm, which is room for the catch. The wall there is free, so ours are longer.
slot_depth   = 10.0;      // 8 was enough for a 0.5 mm catch; 10 (~1.4 mm) leaves room for 0.8
snap_catches = true;
catch_h      = 0.8;       // how far the catch stands in from the wall (0.5 at first, deeper for a firmer grip)
catch_len    = 1.5;       // along z, from the rim; with a lead-in ramp
// Windows in the front plate right over the tongues, as on the original (the
// moulding windows for the catches, front-face scan). Here: push the catch
// back with a small screwdriver to take the adapter off, instead of breaking it.
// Off: under the cap, and with the screws coming from behind, there is no way
// to reach them once the adapter is on the meter.
release_windows = false;
release_win  = [[-30.95, -7.07, -27.73, -2.58], [27.73, 1.65, 30.86, 6.22]];  // [x0, y0, x1, y1]


/* [Screws: M2 self-tapping] */
// From behind, through the adapter front into bosses in the cap: the adapter
// front stays flat and the heads sit hidden inside the adapter.
screws       = [[-7, -25.5], [26, -2]];  // clear of the board, the sensor and the USB-C plug
screw_pilot  = 1.7;
screw_clear  = 2.4;
screw_head   = 4.2;
boss_d       = 5.5;

// --------------------------------------------------------------- derived --

function x_on_circle(r, y) = sqrt(r * r - y * y);
inner_wall_y = edge_y - wall - inner_wall_gap - wall / 2;   // centre line of the inner wall
board_top = cap_inner - board_stand;                 // in cap coordinates (0 = adapter front)
board_bot = board_top - 1.0;

pocket_r = (pocket_d + pocket_clear) / 2;
sensor_c = pocket_c + sensor_off * [cos(sensor_dir), sin(sensor_dir)];

// --------------------------------------------------------------- helpers --

// The blank-module outline: the round, cut by the straight edge, plus the nose.
module outline2d() {
  intersection() {
    circle(r = R);
    polygon([[-R - 1, -R - 1], [R + 1, -R - 1], [R + 1, edge_y + nose_h],
             [nose_x1, edge_y + nose_h], [nose_x0, edge_y], [-R - 1, edge_y]]);
  }
}

// The same shape from z = 0 to h, and its inside with walls of w.
module body(h) { linear_extrude(h) outline2d(); }
module body_inside(w, h) { linear_extrude(h) offset(delta = -w) outline2d(); }

// The three ribs at the bottom of the round, radial plates thinning towards
// the rim, as on the original (probably stops against the register).
module ribs() {
  z0 = max(0.3, D - front - rib[3]);
  for (x = ribs_x) {
    yw = -sqrt((R - wall) * (R - wall) - x * x);
    hull() {
      translate([x - rib[0] / 2, yw - 0.5, D - front - 0.1]) cube([rib[0], rib[2] + 0.5, 0.1]);
      translate([x - rib[1] / 2, yw - 0.5, z0]) cube([rib[1], rib[2] + 0.5, 0.1]);
    }
  }
}

// One snap tongue: two slits from the rim up to slot_depth leave a tongue
// that is free at the rim, and a catch on its inside with a lead-in ramp.
// side = 1 on the right, -1 on the left.
module snap_cut(s, side) {
  xo = side > 0 ? R - wall - 0.8 : -R - 1;
  for (yy = [[s[0], s[2]], [s[3], s[1]]])
    translate([xo, yy[0], -1]) cube([wall + 1.8, yy[1] - yy[0], slot_depth + 1]);
}
module snap_catch(s, side) {
  ym = (s[2] + s[3]) / 2;
  xi = side * sqrt((R - wall) * (R - wall) - ym * ym);   // inner face of the wall
  w  = s[3] - s[2] - 0.2;
  hull() {
    translate([side > 0 ? xi - 0.05 : xi - 0.3, s[2] + 0.1, 0]) cube([0.35, w, 0.05]);
    translate([side > 0 ? xi - catch_h : xi - 0.3, s[2] + 0.1, catch_len - 0.05])
      cube([catch_h + 0.3, w, 0.05]);
  }
}

// The press-fit slot for the sensor, through the front plate and a sleeve
// behind it, open into the pocket below, so the sensor slides back until it
// rests on the window and finds its own height. The sleeve fills the room
// gained by lift, so it ends at the level of the original floor and stays
// clear of the window; printed front down it grows up from the plate.
sleeve_wall = 1.2;
module at_sensor() {
  translate([sensor_c.x, sensor_c.y, D - front]) rotate([0, 0, sensor_dir]) rotate([tilt, 0, 0]) children();
}
module sensor_slot() {
  at_sensor() translate([-(tcrt.x - 2 * press) / 2, -(tcrt.y - 2 * press) / 2, -20])
    cube([tcrt.x - 2 * press, tcrt.y - 2 * press, D + 20]);
}
// Stops at the lower end of the slot so the sensor, pushed in from the front,
// cannot slip through: a 45-degree wedge across each short end. The lens end of
// the TCRT5000 is narrower (about 6.8 mm), its shoulders widen to the full
// 10.2 mm -- they come to rest on the wedges. The window can still push the
// sensor back towards the front.
stop = 1.0;     // how far each wedge reaches in, and how high it is (45 degrees)
module sensor_stops() {
  sx = (tcrt.x - 2 * press) / 2;
  sy = (tcrt.y - 2 * press) / 2;
  at_sensor() for (side = [-1, 1])
    // 0.8 mm up from the sleeve's end, which the tilt makes uneven; 0.3 mm into the wall
    translate([side * sx, -sy, -lift + 0.8]) rotate([-90, 0, 0])
      linear_extrude(2 * sy) polygon([[side * 0.3, 0], [-side * stop, 0], [0, -stop], [side * 0.3, -stop]]);
}

module sensor_sleeve() {
  intersection() {
    at_sensor() translate([-tcrt.x / 2 - sleeve_wall, -tcrt.y / 2 - sleeve_wall, -lift - 3])
      cube([tcrt.x + 2 * sleeve_wall, tcrt.y + 2 * sleeve_wall, lift + 3 + front]);
    translate([-R, -R, D - front - lift]) cube([2 * R, 2 * R, lift + front]);
  }
}

// One corner bracket, hanging from the lid: an L around the board corner with
// a lip at its tip that the board snaps past. cx, cy pick the corner.
module board_bracket(cx, cy) {
  l = bracket[0]; w = bracket[1];
  bx = board_pos.x + cx * board.x;
  by = board_pos.y + cy * board.y;
  z0 = board_bot - lip[1];
  translate([bx, by, 0]) {
    h = cap_inner - z0 + 0.5;   // into the lid
    translate([0, 0, z0]) {
      translate([cx ? fit : -fit - w, cy ? -l : 0, 0]) cube([w, l, h]);
      translate([cx ? -l : 0, cy ? fit : -fit - w, 0]) cube([l, w, h]);
      translate([cx ? fit : -fit - w, cy ? fit : -fit - w, 0]) cube([w, w, h]);
    }
    // lips on both legs: flat towards the lid, a ramp towards the open side
    for (leg = [0, 1]) {
      hull() {
        // at the board's underside: reaching lip[0] in under the board
        translate([leg == 0 ? (cx ? fit - lip[0] : -fit) : (cx ? -l : 0),
                   leg == 1 ? (cy ? fit - lip[0] : -fit) : (cy ? -l : 0), board_bot - 0.05])
          cube([leg == 0 ? lip[0] : l, leg == 1 ? lip[0] : l, 0.05]);
        // at the tip: flush with the wall
        translate([leg == 0 ? (cx ? fit : -fit - 0.01) : (cx ? -l : 0),
                   leg == 1 ? (cy ? fit : -fit - 0.01) : (cy ? -l : 0), z0])
          cube([leg == 0 ? 0.01 : l, leg == 1 ? 0.01 : l, 0.05]);
      }
    }
  }
}

// ------------------------------------------------------------------ adapter --

// Built up from parts rather than carved from a solid: every wall gets its own
// height, and no cut can reach into a wall it was not meant for.
module shell_ring2d() { difference() { outline2d(); offset(delta = -wall) outline2d(); } }
// The round: the only wall that runs the full depth and sits on the meter.
module round_wall2d() { difference() { shell_ring2d(); circle(r = R - wall); } }
// Straight edge, chamfer and top of the nose: the rim that is edge_lip deep.
module edge_wall2d() { intersection() { shell_ring2d(); circle(r = R - wall + 0.01); } }
// Everything inside the walls, where cuts may go.
module inside2d() { offset(delta = -wall) outline2d(); }

module adapter() {
  sensor_stops();
  if (snap_catches) {
    snap_catch(snap_right, 1);
    snap_catch(snap_left, -1);
  }
  difference() {
    union() {
      translate([0, 0, D - front]) linear_extrude(front) outline2d();
      linear_extrude(D) round_wall2d();
      translate([0, 0, D - edge_lip]) linear_extrude(edge_lip) edge_wall2d();
      // The inner wall, inner_wall_gap behind the rim, up to where the nose begins.
      if (inner_wall_on) intersection() {
        translate([-R, inner_wall_y - wall / 2, D - straight_h]) cube([R + nose_x0, wall, straight_h]);
        linear_extrude(D) offset(delta = -wall + 0.5) outline2d();   // runs 0.5 mm into the round
      }
      // Pocket wall, hanging from the front plate, kept inside the outline.
      if (pocket_wall) intersection() {
        translate([pocket_c.x, pocket_c.y, D - front - pocket_wall_h])
          cylinder(r = pocket_r + wall, h = pocket_wall_h + front);
        linear_extrude(D) inside2d();
      }
      if (ribs_on) ribs();
      sensor_sleeve();
    }
    // Room for the window of the scanning disc -- inside the walls only, so
    // the rim of the nose keeps its full depth above it.
    intersection() {
      translate([pocket_c.x, pocket_c.y, -1]) cylinder(r = pocket_r, h = D - front - lift + 1);
      translate([0, 0, -1]) linear_extrude(D + 2) inside2d();
    }
    sensor_slot();
    snap_cut(snap_right, 1);
    snap_cut(snap_left, -1);
    if (release_windows) for (w = release_win)
      translate([w[0], w[1], D - front - 1]) cube([w[2] - w[0], w[3] - w[1], front + 2]);
    for (s = screws)
      translate([s.x, s.y, D - front - 1]) cylinder(d = screw_clear, h = front + 2);
    // Wire pass-through from the sensor into the compartment is the slot itself.
  }
}

// ---------------------------------------------------------------------- cap --

module cap() {
  translate([0, 0, D]) difference() {
    union() {
      difference() {
        body(cap_inner + lid);
        translate([0, 0, -1]) body_inside(cap_wall, cap_inner + 1);
      }
      // Ribs from the lid that the board rests against, along both pin rows,
      // broken where a wire is soldered on: GND, 3V3 (5V row) and GPIO1.
      for (r = ribs) translate([board_pos.x + r[1], r[0] ? board_pos.y + board.y - 1.3 : board_pos.y + 0.3, board_top])
        cube([r[2] - r[1], 1, board_stand + 0.5]);
      for (cx = [0, 1], cy = [0, 1]) board_bracket(cx, cy);
      // Bosses for the screws that come from behind through the adapter front.
      for (s = screws) translate([s.x, s.y, 0]) cylinder(d = boss_d, h = cap_inner + 0.5);

    }
    // Opening for the USB-C plug through the left wall, open towards the adapter.
    translate([-R - 1, board_pos.y + board.y / 2 - usb_notch.x / 2, -1])
      cube([R + board_pos.x - usb_over - 1, usb_notch.x, usb_notch.y + 1]);
    for (b = [boot_pos, rst_pos])
      translate([b.x, b.y, cap_inner - 1]) cylinder(d = button_d, h = lid + 2);
    for (s = screws)
      translate([s.x, s.y, -1]) cylinder(d = screw_pilot, h = 8);
  }
}

// ---------------------------------------------------------------- slot test --

// Three slots side by side, 0.1 mm apart in undersize, marked with 1, 2 or 3
// notches. Push the sensor in: the right one takes it with a firm push and
// holds it upside down.
test_press = [press - 0.1, press, press + 0.1];

module slot_test() {
  difference() {
    cube([3 * (tcrt.x + 4) + 4, tcrt.y + 8, 6]);
    for (i = [0 : 2]) {
      translate([4 + i * (tcrt.x + 4) + test_press[i], 4 + test_press[i], -1])
        cube([tcrt.x - 2 * test_press[i], tcrt.y - 2 * test_press[i], 8]);
      for (n = [0 : i])
        translate([4 + i * (tcrt.x + 4) + n * 2, -1, 5]) cube([1, 2, 2]);
    }
  }
}

// ------------------------------------------------------------------- output --

if (part == "slot-test") {
  slot_test();
} else if (part == "snap-test") {
  // The lowest part of the adapter: the rim with both snap tongues, to try
  // the fit on the meter head in a few minutes of printing. Rim on the bed.
  // The tongues and a little wall above; below the inner wall if that is on, which would float in the ring.
  intersection() { adapter(); translate([-R - 5, -R - 5, 0])
    cube([2 * R + 10, 2 * R + 10, inner_wall_on ? D - straight_h - 0.2 : slot_depth + 2]); }
} else if (part == "adapter") {
  // Front face down.
  translate([0, 0, D]) rotate([180, 0, 0]) adapter();
} else if (part == "cap") {
  // Lid down.
  translate([0, 0, D + cap_inner + lid]) rotate([180, 0, 0]) cap();
} else if (part == "print") {
  translate([0, 0, D]) rotate([180, 0, 0]) adapter();
  translate([0, 2 * R + 10, D + cap_inner + lid]) rotate([180, 0, 0]) cap();
} else {
  color("DimGray") adapter();
  if (part != "open") color("SlateGray", 0.6) cap();
  // Board and sensor, for checking the fit.
  %translate([board_pos.x, board_pos.y, D + board_bot]) cube(board);
  %at_sensor() translate([-tcrt.x / 2, -tcrt.y / 2, -2]) cube(tcrt);
}
