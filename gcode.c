/*
  gcode.c - rs274/ngc parser.
  Part of Horus Firmware

  Copyright (c) 2014-2015 Mundo Reader S.L.

  Horus Firmware is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Horus Firmware is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Horus Firmware.  If not, see <http://www.gnu.org/licenses/>.
*/
/* 
  This file is based on work from Grbl v0.9, distributed under the 
  terms of the GPLv3. See COPYING for more details.
    Copyright (c) 2009-2011 Simen Svale Skogsrud
    Copyright (c) 2011-2014 Sungeun K. Jeon
*/

#include "system.h"
#include "settings.h"
#include "protocol.h"
#include "gcode.h"
#include "motion_control.h"
#include "probe.h"
#include "report.h"

// NOTE: Max line number is defined by the g-code standard to be 99999. It seems to be an
// arbitrary value, and some GUIs may require more. So we increased it based on a max safe
// value when converting a float (7.2 digit precision)s to an integer.
#define MAX_LINE_NUMBER 9999999 

#define AXIS_COMMAND_NONE 0
#define AXIS_COMMAND_NON_MODAL 1 
#define AXIS_COMMAND_MOTION_MODE 2
#define AXIS_COMMAND_TOOL_LENGTH_OFFSET 3 // *Undefined but required

// Declare gc extern struct
parser_state_t gc_state;
parser_block_t gc_block;

#define FAIL(status) return(status);


void gc_init() 
{
  memset(&gc_state, 0, sizeof(gc_state));
  
  // Load default G54 coordinate system.
  if (!(settings_read_coord_data(gc_state.modal.coord_select,gc_state.coord_system))) { 
    report_status_message(STATUS_SETTING_READ_FAIL); 
  } 
}


// Sets g-code parser position in mm. Input in steps. Called by the system abort and hard
// limit pull-off routines.
void gc_sync_position() 
{
  uint8_t i;
  for (i=0; i<N_AXIS; i++) {
    gc_state.position[i] = sys.position[i]/settings.steps_per_deg[i];
  }
}


static uint8_t gc_check_same_position(float *pos_a, float *pos_b) 
{
  uint8_t idx;
  for (idx=0; idx<N_AXIS; idx++) {
    if (pos_a[idx] != pos_b[idx]) { return(false); }
  }
  return(true);
}
         
// Executes one line of 0-terminated G-Code. The line is assumed to contain only uppercase
// characters and signed floating point values (no whitespace). Comments and block delete
// characters have been removed. In this function, all units and positions are converted and 
// exported to grbl's internal functions in terms of (mm, mm/min) and absolute machine 
// coordinates, respectively.
uint8_t gc_execute_line(char *line) 
{
  /* -------------------------------------------------------------------------------------
     STEP 1: Initialize parser block struct and copy current g-code state modes. The parser
     updates these modes and commands as the block line is parser and will only be used and
     executed after successful error-checking. The parser block struct also contains a block
     values struct, word tracking variables, and a non-modal commands tracker for the new 
     block. This struct contains all of the necessary information to execute the block. */
     
  memset(&gc_block, 0, sizeof(gc_block)); // Initialize the parser block struct.
  memcpy(&gc_block.modal,&gc_state.modal,sizeof(gc_modal_t)); // Copy current modes
  uint8_t axis_command = AXIS_COMMAND_NONE;
  uint8_t axis_0, axis_1, axis_linear;
  uint8_t coord_select = 0; // Tracks G10 P coordinate selection for execution
  float coordinate_data[N_AXIS]; // Multi-use variable to store coordinate data for execution
  float parameter_data[N_AXIS]; // Multi-use variable to store parameter data for execution
  
  // Initialize bitflag tracking variables for axis indices compatible operations.
  uint8_t axis_words = 0; // XYZ tracking

  // Initialize command and value words variables. Tracks words contained in this block.
  uint16_t command_words = 0; // G and M command words. Also used for modal group violations.
  uint16_t value_words = 0; // Value words. 

  /* -------------------------------------------------------------------------------------
     STEP 2: Import all g-code words in the block line. A g-code word is a letter followed by
     a number, which can either be a 'G'/'M' command or sets/assigns a command value. Also, 
     perform initial error-checks for command word modal group violations, for any repeated
     words, and for negative values set for the value words F, N, P, T, and S. */
     
  uint8_t word_bit; // Bit-value for assigning tracking variables
  uint8_t char_counter = 0;  
  char letter;
  float value;
  uint8_t int_value = 0;
  uint8_t mantissa = 0; // NOTE: For mantissa values > 255, variable type must be changed to uint16_t.


  while (line[char_counter] != 0) { // Loop until no more g-code words in line.
    
    // Import the next g-code word, expecting a letter followed by a value. Otherwise, error out.
    letter = line[char_counter];
    if((letter < 'A') || (letter > 'Z')) { FAIL(STATUS_EXPECTED_COMMAND_LETTER); } // [Expected word letter]
    char_counter++;
    if (!read_float(line, &char_counter, &value)) { FAIL(STATUS_BAD_NUMBER_FORMAT); } // [Expected word value]

    // Convert values to smaller uint8 significand and mantissa values for parsing this word.
    // NOTE: Mantissa is multiplied by 100 to catch non-integer command values. This is more 
    // accurate than the NIST gcode requirement of x10 when used for commands, but not quite
    // accurate enough for value words that require integers to within 0.0001. This should be
    // a good enough comprimise and catch most all non-integer errors. To make it compliant, 
    // we would simply need to change the mantissa to int16, but this add compiled flash space.
    // Maybe update this later. 
    int_value = trunc(value);
    mantissa =  round(100*(value - int_value)); // Compute mantissa for Gxx.x commands.
    // NOTE: Rounding must be used to catch small floating point errors. 
    // Check if the g-code word is supported or errors due to modal group violations or has
    // been repeated in the g-code block. If ok, update the command or record its value.
    switch(letter) {
    
      /* 'G' and 'M' Command Words: Parse commands and check for modal group violations.
         NOTE: Modal group numbers are defined in Table 4 of NIST RS274-NGC v3, pg.20 */
         
      case 'G':
        // Determine 'G' command and its modal group
        switch(int_value) {
          case 1:
            if (axis_command) { FAIL(STATUS_GCODE_AXIS_COMMAND_CONFLICT); } // [Axis word/command conflict]
            axis_command = AXIS_COMMAND_MOTION_MODE; 
            gc_block.modal.motion = MOTION_MODE_LINEAR;
            word_bit = MODAL_GROUP_G1; 

            break;
          default: FAIL(STATUS_GCODE_UNSUPPORTED_COMMAND); // [Unsupported G command]
        }  
        if (mantissa > 0) { FAIL(STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER); } // [Unsupported or invalid Gxx.x command]
        // Check for more than one command per modal group violations in the current block
        // NOTE: Variable 'word_bit' is always assigned, if the command is valid.
        if ( bit_istrue(command_words,bit(word_bit)) ) { FAIL(STATUS_GCODE_MODAL_GROUP_VIOLATION); }
        command_words |= bit(word_bit);
        break;
        
      case 'M':
        // Determine 'M' command and its modal group
        if (mantissa > 0) { FAIL(STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER); } // [No Mxx.x commands]
        switch(int_value) {
          case 0: case 2:
            word_bit = MODAL_GROUP_M4; 
            switch(int_value) {
              case 0: gc_block.modal.program_flow = PROGRAM_FLOW_PAUSED; break; // Program pause
              case 2: gc_block.modal.program_flow = PROGRAM_FLOW_COMPLETED; break; // Program end and reset 
            }
            break;
          case 17: gc_block.modal.motor = MOTOR_ENABLE; break;
          case 18: gc_block.modal.motor = MOTOR_DISABLE; break;
          case 50: gc_block.modal.ldr = LDR_READ; break;
          case 70: gc_block.modal.laser = LASER_DISABLE; break;
          case 71: gc_block.modal.laser = LASER_ENABLE; break;

          default: FAIL(STATUS_GCODE_UNSUPPORTED_COMMAND); // [Unsupported M command]
        }            
      
        // Check for more than one command per modal group violations in the current block
        // NOTE: Variable 'word_bit' is always assigned, if the command is valid.
        if ( bit_istrue(command_words,bit(word_bit)) ) { FAIL(STATUS_GCODE_MODAL_GROUP_VIOLATION); }
        command_words |= bit(word_bit);
        break;
      
      // NOTE: All remaining letters assign values.
      default: 
  
        /* Non-Command Words: This initial parsing phase only checks for repeats of the remaining
           legal g-code words and stores their value. Error-checking is performed later since some
           words (I,J,K,L,P,R) have multiple connotations and/or depend on the issued commands. */
        switch(letter){
          case 'F': word_bit = WORD_F; gc_block.values.f = value; break;
          case 'T': word_bit = WORD_T; gc_block.values.t = int_value; break; // gc.values.t = int_value;
          case 'X': word_bit = WORD_X; gc_block.values.xyz[X_AXIS] = value; axis_words |= (1<<X_AXIS); break;
          /*case 'Y': word_bit = WORD_Y; gc_block.values.xyz[Y_AXIS] = value; axis_words |= (1<<Y_AXIS); break;
          case 'Z': word_bit = WORD_Z; gc_block.values.xyz[Z_AXIS] = value; axis_words |= (1<<Z_AXIS); break;*/
          default: FAIL(STATUS_GCODE_UNSUPPORTED_COMMAND);
        } 
        
        // NOTE: Variable 'word_bit' is always assigned, if the non-command letter is valid.
        if (bit_istrue(value_words,bit(word_bit))) { FAIL(STATUS_GCODE_WORD_REPEATED); } // [Word repeated]
        // Check for invalid negative values for words F, N, P, T, and S.
        // NOTE: Negative value check is done here simply for code-efficiency.
        if ( bit(word_bit) & (bit(WORD_F)|bit(WORD_N)|bit(WORD_P)|bit(WORD_T)|bit(WORD_S)) ) {
          if (value < 0.0) { FAIL(STATUS_NEGATIVE_VALUE); } // [Word value cannot be negative]
        }
        value_words |= bit(word_bit); // Flag to indicate parameter assigned.
      
    }   
  } 
  // Parsing complete!
  

  /* -------------------------------------------------------------------------------------
     STEP 3: Error-check all commands and values passed in this block. This step ensures all of
     the commands are valid for execution and follows the NIST standard as closely as possible.
     If an error is found, all commands and values in this block are dumped and will not update
     the active system g-code modes. If the block is ok, the active system g-code modes will be
     updated based on the commands of this block, and signal for it to be executed. 
     
     Also, we have to pre-convert all of the values passed based on the modes set by the parsed
     block. There are a number of error-checks that require target information that can only be
     accurately calculated if we convert these values in conjunction with the error-checking.
     This relegates the next execution step as only updating the system g-code modes and 
     performing the programmed actions in order. The execution step should not require any 
     conversion calculations and would only require minimal checks necessary to execute.
  */

  /* NOTE: At this point, the g-code block has been parsed and the block line can be freed.
     NOTE: It's also possible, at some future point, to break up STEP 2, to allow piece-wise 
     parsing of the block on a per-word basis, rather than the entire block. This could remove 
     the need for maintaining a large string variable for the entire block and free up some memory. 
     To do this, this would simply need to retain all of the data in STEP 1, such as the new block
     data struct, the modal group and value bitflag tracking variables, and axis array indices 
     compatible variables. This data contains all of the information necessary to error-check the 
     new g-code block when the EOL character is received. However, this would break Grbl's startup
     lines in how it currently works and would require some refactoring to make it compatible.
  */  
  
  // [0. Non-specific/common error-checks and miscellaneous setup]: 
  
  // Determine implicit axis command conditions. Axis words have been passed, but no explicit axis
  // command has been sent. If so, set axis command to current motion mode.
  if (axis_words) {
    if (!axis_command) { axis_command = AXIS_COMMAND_MOTION_MODE; } // Assign implicit motion-mode
  }
  
  // Check for valid line number N value.
  if (bit_istrue(value_words,bit(WORD_N))) {
    // Line number value cannot be less than zero (done) or greater than max line number.
    if (gc_block.values.n > MAX_LINE_NUMBER) { FAIL(STATUS_GCODE_INVALID_LINE_NUMBER); } // [Exceeds max line number]
  }
  // bit_false(value_words,bit(WORD_N)); // NOTE: Single-meaning value word. Set at end of error-checking.
  
  // Track for unused words at the end of error-checking.
  // NOTE: Single-meaning value words are removed all at once at the end of error-checking, because
  // they are always used when present. This was done to save a few bytes of flash. For clarity, the
  // single-meaning value words may be removed as they are used. Also, axis words are treated in the
  // same way. If there is an explicit/implicit axis command, XYZ words are always used and are 
  // are removed at the end of error-checking.  
  
  // [1. Comments ]: MSG's NOT SUPPORTED. Comment handling performed by protocol.
  
  // [2. Set feed rate mode ]: G93 F word missing with G1,G2/3 active, implicitly or explicitly. Feed rate
  //   is not defined after switching to G94 from G93.
  if (gc_block.modal.feed_rate == FEED_RATE_MODE_INVERSE_TIME) { // = G93
    // NOTE: G38 can also operate in inverse time, but is undefined as an error. Missing F word check added here.
    if (axis_command == AXIS_COMMAND_MOTION_MODE) { 
      if ((gc_block.modal.motion != MOTION_MODE_NONE) || (gc_block.modal.motion != MOTION_MODE_SEEK)) {
        if (bit_isfalse(value_words,bit(WORD_F))) { FAIL(STATUS_GCODE_UNDEFINED_FEED_RATE); } // [F word missing]
      }
    }
    // NOTE: It seems redundant to check for an F word to be passed after switching from G94 to G93. We would
    // accomplish the exact same thing if the feed rate value is always reset to zero and undefined after each
    // inverse time block, since the commands that use this value already perform undefined checks. This would
    // also allow other commands, following this switch, to execute and not error out needlessly. This code is 
    // combined with the above feed rate mode and the below set feed rate error-checking.

    // [3. Set feed rate ]: F is negative (done.)
    // - In inverse time mode: Always implicitly zero the feed rate value before and after block completion.
    // NOTE: If in G93 mode or switched into it from G94, just keep F value as initialized zero or passed F word 
    // value in the block. If no F word is passed with a motion command that requires a feed rate, this will error 
    // out in the motion modes error-checking. However, if no F word is passed with NO motion command that requires
    // a feed rate, we simply move on and the state feed rate value gets updated to zero and remains undefined.
  } else { // = G94
    // - In units per mm mode: If F word passed, ensure value is in mm/min, otherwise push last state value.
    if (gc_state.modal.feed_rate == FEED_RATE_MODE_UNITS_PER_MIN) { // Last state is also G94
      if (bit_istrue(value_words,bit(WORD_F))) {
        if (gc_block.modal.units == UNITS_MODE_INCHES) { gc_block.values.f *= MM_PER_INCH; }
      } else {
        gc_block.values.f = gc_state.feed_rate/60; // Push last state feed rate. Convert deg/min to deg/sec
      }
    } // Else, switching to G94 from G93, so don't push last state feed rate. Its undefined or the passed F word value.
  } 
  // bit_false(value_words,bit(WORD_F)); // NOTE: Single-meaning value word. Set at end of error-checking.
  
  // [4. Set spindle speed ]: S is negative (done.)
  if (bit_isfalse(value_words,bit(WORD_S))) { gc_block.values.s = gc_state.spindle_speed; }
  // bit_false(value_words,bit(WORD_S)); // NOTE: Single-meaning value word. Set at end of error-checking.
    
  // [5. Select tool ]: NOT SUPPORTED. Only tracks value. T is negative (done.) Not an integer. Greater than max tool value.
  // bit_false(value_words,bit(WORD_T)); // NOTE: Single-meaning value word. Set at end of error-checking.

  // [6. Change tool ]: N/A
  // [7. Spindle control ]: N/A
  // [8. Coolant control ]: N/A
  // [9. Enable/disable feed rate or spindle overrides ]: NOT SUPPORTED.
  
  // [10. Dwell ]: P value missing. P is negative (done.) NOTE: See below.
  if (gc_block.non_modal_command == NON_MODAL_DWELL) {
    if (bit_isfalse(value_words,bit(WORD_P))) { FAIL(STATUS_GCODE_VALUE_WORD_MISSING); } // [P word missing]
    bit_false(value_words,bit(WORD_P));
  }
  
  // [11. Set active plane ]: N/A
  switch (gc_block.modal.plane_select) {
    case PLANE_SELECT_XY:
      axis_0 = X_AXIS;
      axis_1 = Y_AXIS;
      axis_linear = Z_AXIS;
      break;
    case PLANE_SELECT_ZX:
      axis_0 = Z_AXIS;
      axis_1 = X_AXIS;
      axis_linear = Y_AXIS;
      break;
    default: // case PLANE_SELECT_YZ:
      axis_0 = Y_AXIS;
      axis_1 = Z_AXIS;
      axis_linear = X_AXIS;
  }   
            
  // [12. Set length units ]: N/A
  // Pre-convert XYZ coordinate values to millimeters, if applicable.
  uint8_t idx;
  if (gc_block.modal.units == UNITS_MODE_INCHES) {
    for (idx=0; idx<N_AXIS; idx++) { // Axes indices are consistent, so loop may be used.
      if (bit_istrue(axis_words,bit(idx)) ) {
        gc_block.values.xyz[idx] *= MM_PER_INCH;
      }
    }
  }
  
  // [13. Cutter radius compensation ]: NOT SUPPORTED. Error, if G53 is active.
  
  // [14. Cutter length compensation ]: G43 NOT SUPPORTED, but G43.1 and G49 are. 
  // [G43.1 Errors]: Motion command in same line. 
  //   NOTE: Although not explicitly stated so, G43.1 should be applied to only one valid 
  //   axis that is configured (in config.h). There should be an error if the configured axis
  //   is absent or if any of the other axis words are present.
  if (axis_command == AXIS_COMMAND_TOOL_LENGTH_OFFSET ) { // Indicates called in block.
    if (gc_block.modal.tool_length == TOOL_LENGTH_OFFSET_ENABLE_DYNAMIC) {
      if (axis_words ^ (1<<TOOL_LENGTH_OFFSET_AXIS)) { FAIL(STATUS_GCODE_G43_DYNAMIC_AXIS_ERROR); }
    }
  }
  
  // [15. Coordinate system selection ]: *N/A. Error, if cutter radius comp is active.
  // TODO: An EEPROM read of the coordinate data may require a buffer sync when the cycle
  // is active. The read pauses the processor temporarily and may cause a rare crash. For 
  // future versions on processors with enough memory, all coordinate data should be stored
  // in memory and written to EEPROM only when there is not a cycle active.
  memcpy(coordinate_data,gc_state.coord_system,sizeof(gc_state.coord_system));
  if ( bit_istrue(command_words,bit(MODAL_GROUP_G12)) ) { // Check if called in block
    if (gc_block.modal.coord_select > N_COORDINATE_SYSTEM) { FAIL(STATUS_GCODE_UNSUPPORTED_COORD_SYS); } // [Greater than N sys]
    if (gc_state.modal.coord_select != gc_block.modal.coord_select) {
      if (!(settings_read_coord_data(gc_block.modal.coord_select,coordinate_data))) { FAIL(STATUS_SETTING_READ_FAIL); } 
    }
  }
  
  // [16. Set path control mode ]: NOT SUPPORTED.
  // [17. Set distance mode ]: N/A. G90.1 and G91.1 NOT SUPPORTED.
  // [18. Set retract mode ]: NOT SUPPORTED.
  
  // [19. Remaining non-modal actions ]: Check go to predefined position, set G10, or set axis offsets.
  // NOTE: We need to separate the non-modal commands that are axis word-using (G10/G28/G30/G92), as these
  // commands all treat axis words differently. G10 as absolute offsets or computes current position as
  // the axis value, G92 similarly to G10 L20, and G28/30 as an intermediate target position that observes
  // all the current coordinate system and G92 offsets. 
  switch (gc_block.non_modal_command) {
    case NON_MODAL_SET_COORDINATE_DATA:  
      // [G10 Errors]: L missing and is not 2 or 20. P word missing. (Negative P value done.)
      // [G10 L2 Errors]: R word NOT SUPPORTED. P value not 0 to nCoordSys(max 9). Axis words missing.
      // [G10 L20 Errors]: P must be 0 to nCoordSys(max 9). Axis words missing.
      if (!axis_words) { FAIL(STATUS_GCODE_NO_AXIS_WORDS) }; // [No axis words]
      if (bit_isfalse(value_words,((1<<WORD_P)|(1<<WORD_L)))) { FAIL(STATUS_GCODE_VALUE_WORD_MISSING); } // [P/L word missing]
      coord_select = trunc(gc_block.values.p); // Convert p value to int.
      if (coord_select > N_COORDINATE_SYSTEM) { FAIL(STATUS_GCODE_UNSUPPORTED_COORD_SYS); } // [Greater than N sys]
      if (gc_block.values.l != 20) {
        if (gc_block.values.l == 2) {
          if (bit_istrue(value_words,bit(WORD_R))) { FAIL(STATUS_GCODE_UNSUPPORTED_COMMAND); } // [G10 L2 R not supported]
        } else { FAIL(STATUS_GCODE_UNSUPPORTED_COMMAND); } // [Unsupported L]
      }
      bit_false(value_words,(bit(WORD_L)|bit(WORD_P)));
      
      // Determine coordinate system to change and try to load from EEPROM.
      if (coord_select > 0) { coord_select--; } // Adjust P1-P6 index to EEPROM coordinate data indexing.
      else { coord_select = gc_block.modal.coord_select; } // Index P0 as the active coordinate system
      if (!settings_read_coord_data(coord_select,parameter_data)) { FAIL(STATUS_SETTING_READ_FAIL); } // [EEPROM read fail]
    
      // Pre-calculate the coordinate data changes. NOTE: Uses parameter_data since coordinate_data may be in use by G54-59.
      for (idx=0; idx<N_AXIS; idx++) { // Axes indices are consistent, so loop may be used.
        // Update axes defined only in block. Always in machine coordinates. Can change non-active system.
        if (bit_istrue(axis_words,bit(idx)) ) {
          if (gc_block.values.l == 20) {
            // L20: Update coordinate system axis at current position (with modifiers) with programmed value
            parameter_data[idx] = gc_state.position[idx]-gc_state.coord_offset[idx]-gc_block.values.xyz[idx];
            if (idx == TOOL_LENGTH_OFFSET_AXIS) { parameter_data[idx] -= gc_state.tool_length_offset; }
          } else {
            // L2: Update coordinate system axis to programmed value.
            parameter_data[idx] = gc_block.values.xyz[idx]; 
          }
        }
      }
      break;
    case NON_MODAL_SET_COORDINATE_OFFSET:
      // [G92 Errors]: No axis words.
      if (!axis_words) { FAIL(STATUS_GCODE_NO_AXIS_WORDS); } // [No axis words]
    
      // Update axes defined only in block. Offsets current system to defined value. Does not update when
      // active coordinate system is selected, but is still active unless G92.1 disables it. 
      for (idx=0; idx<N_AXIS; idx++) { // Axes indices are consistent, so loop may be used.
        if (bit_istrue(axis_words,bit(idx)) ) {
          gc_block.values.xyz[idx] = gc_state.position[idx]-coordinate_data[idx]-gc_block.values.xyz[idx];
          if (idx == TOOL_LENGTH_OFFSET_AXIS) { gc_block.values.xyz[idx] -= gc_state.tool_length_offset; }
        } else {
          gc_block.values.xyz[idx] = gc_state.coord_offset[idx];
        }
      }
      break;
      
    default:

      // At this point, the rest of the explicit axis commands treat the axis values as the traditional
      // target position with the coordinate system offsets, G92 offsets, absolute override, and distance
      // modes applied. This includes the motion mode commands. We can now pre-compute the target position.
      // NOTE: Tool offsets may be appended to these conversions when/if this feature is added.
      if (axis_command != AXIS_COMMAND_TOOL_LENGTH_OFFSET ) { // TLO block any axis command.
        if (axis_words) {
          for (idx=0; idx<N_AXIS; idx++) { // Axes indices are consistent, so loop may be used to save flash space.
            if ( bit_isfalse(axis_words,bit(idx)) ) {
              gc_block.values.xyz[idx] = gc_state.position[idx]; // No axis word in block. Keep same axis position.
            } else {
              // Update specified value according to distance mode or ignore if absolute override is active.
              // NOTE: G53 is never active with G28/30 since they are in the same modal group.
              if (gc_block.non_modal_command != NON_MODAL_ABSOLUTE_OVERRIDE) {
                // Apply coordinate offsets based on distance mode.
                if (gc_block.modal.distance == DISTANCE_MODE_ABSOLUTE) {
                  gc_block.values.xyz[idx] += coordinate_data[idx] + gc_state.coord_offset[idx];
                  if (idx == TOOL_LENGTH_OFFSET_AXIS) { gc_block.values.xyz[idx] += gc_state.tool_length_offset; }
                } else {  // Incremental mode
                  gc_block.values.xyz[idx] += gc_state.position[idx];
                }
              }
            }
          }
        }
      }
          
      // Check remaining non-modal commands for errors.
      switch (gc_block.non_modal_command) {        
        case NON_MODAL_GO_HOME_0:
          // [G28 Errors]: Cutter compensation is enabled. 
          // Retreive G28 go-home position data (in machine coordinates) from EEPROM
          if (!axis_words) { axis_command = AXIS_COMMAND_NONE; } // Set to none if no intermediate motion.
          if (!settings_read_coord_data(SETTING_INDEX_G28,parameter_data)) { FAIL(STATUS_SETTING_READ_FAIL); }
          break;
        case NON_MODAL_GO_HOME_1:
          // [G30 Errors]: Cutter compensation is enabled. 
          // Retreive G30 go-home position data (in machine coordinates) from EEPROM
          if (!axis_words) { axis_command = AXIS_COMMAND_NONE; } // Set to none if no intermediate motion.
          if (!settings_read_coord_data(SETTING_INDEX_G30,parameter_data)) { FAIL(STATUS_SETTING_READ_FAIL); }
          break;
        case NON_MODAL_SET_HOME_0: case NON_MODAL_SET_HOME_1:
          // [G28.1/30.1 Errors]: Cutter compensation is enabled. 
          // NOTE: If axis words are passed here, they are interpreted as an implicit motion mode.
          break;
        case NON_MODAL_RESET_COORDINATE_OFFSET:
          // NOTE: If axis words are passed here, they are interpreted as an implicit motion mode.
          break;
        case NON_MODAL_ABSOLUTE_OVERRIDE:
          // [G53 Errors]: G0 and G1 are not active. Cutter compensation is enabled.
          // NOTE: All explicit axis word commands are in this modal group. So no implicit check necessary.
          if (!(gc_block.modal.motion == MOTION_MODE_SEEK || gc_block.modal.motion == MOTION_MODE_LINEAR)) {
            FAIL(STATUS_GCODE_G53_INVALID_MOTION_MODE); // [G53 G0/1 not active]
          }
          break;
      }
  }
      
  // [20. Motion modes ]: 
  if (gc_block.modal.motion == MOTION_MODE_NONE) {
    // [G80 Errors]: Axis word exist and are not used by a non-modal command.
    if ((axis_words) && (axis_command != AXIS_COMMAND_NON_MODAL)) { 
      FAIL(STATUS_GCODE_AXIS_WORDS_EXIST); // [No axis words allowed]
    }

  // Check remaining motion modes, if axis word are implicit (exist and not used by G10/28/30/92), or 
  // was explicitly commanded in the g-code block.
  } else if ( axis_command == AXIS_COMMAND_MOTION_MODE ) {
  
    if (gc_block.modal.motion == MOTION_MODE_SEEK) {
      // [G0 Errors]: Axis letter not configured or without real value (done.)
      // Axis words are optional. If missing, set axis command flag to ignore execution.
      if (!axis_words) { axis_command = AXIS_COMMAND_NONE; }

    // All remaining motion modes (all but G0 and G80), require a valid feed rate value. In units per mm mode,
    // the value must be positive. In inverse time mode, a positive value must be passed with each block.
    } else {      
      // Check if feed rate is defined for the motion modes that require it.
      if (gc_block.values.f == 0.0) { FAIL(STATUS_GCODE_UNDEFINED_FEED_RATE); } // [Feed rate undefined]
     
      switch (gc_block.modal.motion) {
        case MOTION_MODE_LINEAR: 
          // [G1 Errors]: Feed rate undefined. Axis letter not configured or without real value.
          // Axis words are optional. If missing, set axis command flag to ignore execution.
          if (!axis_words) { axis_command = AXIS_COMMAND_NONE; }

          break;
        case MOTION_MODE_PROBE:
          // [G38 Errors]: Target is same current. No axis words. Cutter compensation is enabled. Feed rate
          //   is undefined. Probe is triggered. NOTE: Probe check moved to probe cycle. Instead of returning
          //   an error, it issues an alarm to prevent further motion to the probe. It's also done there to 
          //   allow the planner buffer to empty and move off the probe trigger before another probing cycle.
          if (!axis_words) { FAIL(STATUS_GCODE_NO_AXIS_WORDS); } // [No axis words]
          if (gc_check_same_position(gc_state.position, gc_block.values.xyz)) { FAIL(STATUS_GCODE_INVALID_TARGET); } // [Invalid target]
          break;
      } 
    }
  }
  
  // [21. Program flow ]: No error checks required.

  // [0. Non-specific error-checks]: Complete unused value words check,
  // radius mode, or axis words that aren't used in the block.  
  bit_false(value_words,(bit(WORD_N)|bit(WORD_F)|bit(WORD_S)|bit(WORD_T))); // Remove single-meaning value words. 
  if (axis_command) { bit_false(value_words,(bit(WORD_X)|bit(WORD_Y)|bit(WORD_Z))); } // Remove axis words. 
  if (value_words) { FAIL(STATUS_GCODE_UNUSED_WORDS); } // [Unused words]

   
  /* -------------------------------------------------------------------------------------
     STEP 4: EXECUTE!!
     Assumes that all error-checking has been completed and no failure modes exist. We just
     need to update the state and execute the block according to the order-of-execution.
  */ 
  
  // [1. Comments feedback ]:  NOT SUPPORTED
  
  // [2. Set feed rate mode ]:
  gc_state.modal.feed_rate = gc_block.modal.feed_rate;
  
  // [3. Set feed rate ]:
  gc_state.feed_rate = gc_block.values.f*60; // Always copy this value. See feed rate error-checking. Convert deg/sec to deg/min

  /*// [4. Set spindle speed ]:
  if (gc_state.spindle_speed != gc_block.values.s) { 
    gc_state.spindle_speed = gc_block.values.s; 
    
    // Update running spindle only if not in check mode and not already enabled.
    if (gc_state.modal.spindle != SPINDLE_DISABLE) { spindle_run(gc_state.modal.spindle, gc_state.spindle_speed); }
  }*/
    
  // [5. Select tool ]: NOT SUPPORTED. Only tracks tool value.
  gc_state.tool = gc_block.values.t;

  // [6. Change tool ]: NOT SUPPORTED
  // [7. Spindle control ]: NOT SUPPORTED
  // [8. Coolant control ]: NOT SUPPORTED
  // [9. Enable/disable feed rate or spindle overrides ]: NOT SUPPORTED

  // [10. Dwell ]:
  if (gc_block.non_modal_command == NON_MODAL_DWELL) { mc_dwell(gc_block.values.p); }
  
  // [11. Set active plane ]:
  gc_state.modal.plane_select = gc_block.modal.plane_select;  

  // [12. Set length units ]: NOT SUPPORTED
  // [13. Cutter radius compensation ]: NOT SUPPORTED

  // [14. Cutter length compensation ]: G43.1 and G49 supported. G43 NOT SUPPORTED.
  // NOTE: If G43 were supported, its operation wouldn't be any different from G43.1 in terms
  // of execution. The error-checking step would simply load the offset value into the correct
  // axis of the block XYZ value array. 
  if (axis_command == AXIS_COMMAND_TOOL_LENGTH_OFFSET ) { // Indicates a change.
    gc_state.modal.tool_length = gc_block.modal.tool_length;
    if (gc_state.modal.tool_length == TOOL_LENGTH_OFFSET_ENABLE_DYNAMIC) { // G43.1
      gc_state.tool_length_offset = gc_block.values.xyz[TOOL_LENGTH_OFFSET_AXIS];
    } else { // G49
      gc_state.tool_length_offset = 0.0;
    }
  }
  
  // [15. Coordinate system selection ]:
  if (gc_state.modal.coord_select != gc_block.modal.coord_select) {
    gc_state.modal.coord_select = gc_block.modal.coord_select;
    memcpy(gc_state.coord_system,coordinate_data,sizeof(coordinate_data));
  }
  
  // [16. Set path control mode ]: NOT SUPPORTED
  
  // [17. Set distance mode ]:
  gc_state.modal.distance = gc_block.modal.distance;
  
  // [18. Set retract mode ]: NOT SUPPORTED
    
  // [19. Go to predefined position, Set G10, or Set axis offsets ]:
  switch(gc_block.non_modal_command) {
    case NON_MODAL_SET_COORDINATE_DATA:    
      settings_write_coord_data(coord_select,parameter_data);
      // Update system coordinate system if currently active.
      if (gc_state.modal.coord_select == coord_select) { memcpy(gc_state.coord_system,parameter_data,sizeof(parameter_data)); }
      break;
    case NON_MODAL_GO_HOME_0: case NON_MODAL_GO_HOME_1: 
      // Move to intermediate position before going home. Obeys current coordinate system and offsets 
      // and absolute and incremental modes.
      if (axis_command) {
        #ifdef USE_LINE_NUMBERS
          mc_line(gc_block.values.xyz, -1.0, false, gc_block.values.n);
        #else
          mc_line(gc_block.values.xyz, -1.0, false);
        #endif
      }
      #ifdef USE_LINE_NUMBERS
        mc_line(parameter_data, -1.0, false, gc_block.values.n); 
      #else
        mc_line(parameter_data, -1.0, false); 
      #endif
      memcpy(gc_state.position, parameter_data, sizeof(parameter_data));
      break;
    case NON_MODAL_SET_HOME_0: 
      settings_write_coord_data(SETTING_INDEX_G28,gc_state.position);
      break;
    case NON_MODAL_SET_HOME_1:
      settings_write_coord_data(SETTING_INDEX_G30,gc_state.position);
      break;
    case NON_MODAL_SET_COORDINATE_OFFSET:
      memcpy(gc_state.coord_offset,gc_block.values.xyz,sizeof(gc_block.values.xyz));
      break;
    case NON_MODAL_RESET_COORDINATE_OFFSET: 
      clear_vector(gc_state.coord_offset); // Disable G92 offsets by zeroing offset vector.
      break;
  }

  
  // [20. Motion modes ]:
  // NOTE: Commands G10,G28,G30,G92 lock out and prevent axis words from use in motion modes. 
  // Enter motion modes only if there are axis words or a motion mode command word in the block.
  gc_state.modal.motion = gc_block.modal.motion;
    if (axis_command == AXIS_COMMAND_MOTION_MODE) {
      switch (gc_state.modal.motion) {
        case MOTION_MODE_SEEK:
          #ifdef USE_LINE_NUMBERS
            mc_line(gc_block.values.xyz, -1.0, false, gc_block.values.n);
          #else
            mc_line(gc_block.values.xyz, -1.0, false);
          #endif
          break;
        case MOTION_MODE_LINEAR:
          #ifdef USE_LINE_NUMBERS
            mc_line(gc_block.values.xyz, gc_state.feed_rate, gc_state.modal.feed_rate, gc_block.values.n);
          #else
            mc_line(gc_block.values.xyz, gc_state.feed_rate, gc_state.modal.feed_rate);
          #endif
          break;
        case MOTION_MODE_PROBE:
          // NOTE: gc_block.values.xyz is returned from mc_probe_cycle with the updated position value. So
          // upon a successful probing cycle, the machine position and the returned value should be the same.
          #ifdef USE_LINE_NUMBERS
            mc_probe_cycle(gc_block.values.xyz, gc_state.feed_rate, gc_state.modal.feed_rate, gc_block.values.n);
          #else
            mc_probe_cycle(gc_block.values.xyz, gc_state.feed_rate, gc_state.modal.feed_rate);
          #endif
      }
    
      // As far as the parser is concerned, the position is now == target. In reality the
      // motion control system might still be processing the action and the real tool position
      // in any intermediate location.
      memcpy(gc_state.position, gc_block.values.xyz, sizeof(gc_block.values.xyz)); // gc.position[] = target[];
    }
  
  // [21. Program flow ]:
  // M0,M2: Perform non-running program flow actions. During a program pause, the buffer may 
  // refill and can only be resumed by the cycle start run-time command.
  gc_state.modal.program_flow = gc_block.modal.program_flow;
  if (gc_state.modal.program_flow) { 
    protocol_buffer_synchronize(); // Finish all remaining buffered motions. Program paused when complete.
    sys.auto_start = false; // Disable auto cycle start. Forces pause until cycle start issued.
  
    // If complete, reset to reload defaults (G92.2,G54,G17,G90,G94,M48,G40,M5,M9). Otherwise,
    // re-enable program flow after pause complete, where cycle start will resume the program.
    if (gc_state.modal.program_flow == PROGRAM_FLOW_COMPLETED) { mc_reset(); }
    else { gc_state.modal.program_flow = PROGRAM_FLOW_RUNNING; }
  }

  // [22. Laser control ]:  
  gc_state.modal.laser = gc_block.modal.laser;
  laser_run(gc_block.values.t, gc_block.modal.laser);

  // [23. Motor control ]:  
  gc_state.modal.motor = gc_block.modal.motor;
  if (gc_block.modal.motor == MOTOR_ENABLE) {
    st_disable_on_idle(false);
    st_wake_up();
  }
  else {
    st_disable_on_idle(true);
    st_go_idle();
  }
  // [23. LDR read ]:
  if (gc_block.modal.ldr == LDR_READ){
      print_ldr(gc_block.values.t);
  }

  // TODO: % to denote start of program. Sets auto cycle start?
  return(STATUS_OK);
}
        

/* 
  Not supported:

  - Canned cycles
  - Tool radius compensation
  - A,B,C-axes
  - Evaluation of expressions
  - Variables
  - Override control (TBD)
  - Tool changes
  - Switches
   
   (*) Indicates optional parameter, enabled through config.h and re-compile
   group 0 = {G92.2, G92.3} (Non modal: Cancel and re-enable G92 offsets)
   group 1 = {G81 - G89} (Motion modes: Canned cycles)
   group 4 = {M1} (Optional stop, ignored)
   group 6 = {M6} (Tool change)
   group 7 = {G40, G41, G42} cutter radius compensation
   group 8 = {G43} tool length offset (But G43.1/G94 IS SUPPORTED)
   group 8 = {*M7} enable mist coolant
   group 9 = {M48, M49} enable/disable feed and speed override switches
   group 10 = {G98, G99} return mode canned cycles
   group 13 = {G61, G61.1, G64} path control mode
   group 14 = {M50} LDR read
*/
