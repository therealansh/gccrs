------------------------------------------------------------------------------
--                                                                          --
--                         GNAT RUN-TIME COMPONENTS                         --
--                                                                          --
--                      S Y S T E M . I M G _ U T I L                       --
--                                                                          --
--                                 S p e c                                  --
--                                                                          --
--            Copyright (C) 2020, Free Software Foundation, Inc.            --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 3,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.                                     --
--                                                                          --
-- As a special exception under Section 7 of GPL version 3, you are granted --
-- additional permissions described in the GCC Runtime Library Exception,   --
-- version 3.1, as published by the Free Software Foundation.               --
--                                                                          --
-- You should have received a copy of the GNU General Public License and    --
-- a copy of the GCC Runtime Library Exception along with this program;     --
-- see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see    --
-- <http://www.gnu.org/licenses/>.                                          --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

--  This package provides some common utilities used by the s-imgxxx files

package System.Img_Util is
   pragma Pure;

   procedure Set_Decimal_Digits
     (Digs  : in out String;
      NDigs : Natural;
      S     : out String;
      P     : in out Natural;
      Scale : Integer;
      Fore  : Natural;
      Aft   : Natural;
      Exp   : Natural);
   --  Sets the image of Digs (1 .. NDigs), which is a string of decimal digits
   --  preceded by either a minus sign or a space, i.e. the integer image of
   --  the value in units of delta of a decimal fixed point type with the given
   --  Scale, starting at S (P + 1), updating P to point to the last character
   --  stored, the caller promises that the buffer is large enough and no check
   --  is made for this. Constraint_Error will not necessarily be raised if the
   --  requirement is violated since it is perfectly valid to compile this unit
   --  with checks off. The Fore, Aft and Exp values can be set to any valid
   --  values for the case of use by Text_IO.Decimal_IO. Note that there is no
   --  leading space stored. The call may destroy the value in Digs, which is
   --  why Digs is in-out (this happens if rounding is required).

end System.Img_Util;
