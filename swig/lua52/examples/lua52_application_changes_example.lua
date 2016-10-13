-- This sample application demonstrates use of Lua programming language bindings for sysrepo library.
-- Original c application was rewritten in Lua to show similarities and differences
-- between the two.
--
-- Most notable difference is in the very different nature of languages, c is weakly statically typed language while Lua is strongly dynamiclally typed.
-- Lua code is much easier to read and logic easier to comprehend for smaller scripts. Memory safety is not an issue but lower performance can be expectd.

-- The original c implementation is also available in the source, so one can refer to it to evaluate trade-offs.
--

sr = require("libsysrepoLua52")

-- Function for printing out values depending on their type.
function print_value(value)
   local x = value:xpath()
   x = x .. " = "
   if (value:type() == sr.SR_CONTAINER_T) then
      print(x .. "(container)")
   elseif (value:type() == sr.SR_CONTAINER_PRESENCE_T) then
      print(x .. "(container)")
   elseif (value:type() == sr.SR_LIST_T) then
      print(x .. "(list instance)")
   elseif (value:type() == sr.SR_STRING_T) then
      print(x .. value:data():get_string())
   elseif (value:type() == sr.SR_BOOL_T) then
      if (value:data():get_bool()) then
         print(x .. "true")
      else
         print(x .. "false")
      end
   elseif (value:type() == sr.SR_INT8_T) then
      print(x .. value:data():get_int8())
   elseif (value:type() == sr.SR_INT16_T) then
      print(x .. value:data():get_int16())
   elseif (value:type() == sr.SR_INT32_T) then
      print(x .. value:data():get_int32())
   elseif (value:type() == sr.SR_INT64_T) then
      print(x .. value:data():get_int64())
   elseif (value:type() == sr.SR_UINT8_T) then
      print(x .. value:data():get_uint8())
   elseif (value:type() == sr.SR_UINT16_T) then
      print(x .. value:data():get_uint16())
   elseif (value:type() == sr.SR_UINT32_T) then
      print(x .. value:data():get_uint32())
   elseif (value:type() == sr.SR_UINT64_T) then
      print(x .. value:data():get_uint64())
   elseif (value:type() == sr.SR_IDENTITYREF_T) then
      print(x .. value:data():get_identityref())
   elseif (value:type() == sr.SR_BITS_T) then
      print(x .. value:data():get_bits())
   elseif (value:type() == sr.SR_BINARY_T) then
      print(x .. value:data():get_binary())
   else
      print(x .. "(unprintable)")
   end
end

-- Helper function for printing changes given operation, old and new value.
function print_change(op, old_val, new_val)
    if (op == sr.SR_OP_CREATED) then
           print ("CREATED: ")
           print_value(new_val)
    elseif (op == sr.SR_OP_DELETED) then
           print ("DELETED: ")
           print_value(old_val);
    elseif (op == sr.SR_OP_MODIFIED) then
           print ("MODIFIED: ")
           print ("old value")
           print_value(old_val)
           print ("new value")
           print_value(new_val)
    elseif (op == sr.SR_OP_MOVED) then
        print ("MOVED: " .. new_val.get_xpath() .. " after " .. old_val.get_xpath())
    end
end

-- Function to print current configuration state.
-- It does so by loading all the items of a session and printing them out.
function print_current_config(sess, module_name)

    function run()
        xpath = "/" .. module_name .. ":*//*"
        values = sess:get_items(xpath)

	if (values == nil) then return end

	for i=0, values:val_cnt() - 1, 1 do
            print_value(values:val(i))
	end
    end

    ok,res=pcall(run)
    if not ok then
        print("\nerror: ",res, "\n")
    end

end

-- Function to be called for subscribed client of given session whenever configuration changes.
function module_change_cb(sess, module_name, event, private_ctx)
    print("\n\n ========== CONFIG HAS CHANGED, CURRENT RUNNING CONFIG: ==========\n\n")

    function run()
        print_current_config(sess, module_name)

        print("\n\n ========== CONFIG HAS CHANGED, CURRENT RUNNING CONFIG: ==========\n\n")

        print("\n\n ========== CHANGES: =============================================\n\n")

        change_path = "/" .. module_name .. ":*"

        subscribe = sr.Subscribe(sess);
        it = subscribe:get_changes_iter(change_path);

        while true do
            change = subscribe:get_change_next(it)
            if (change == nil) then break end
            print_change(change:oper(), change:old_val(), change:new_val())
	end

	print("\n\n ========== END OF CHANGES =======================================\n\n")
    end

    ok,res=pcall(run)
    if not ok then
        print("\nerror: ",res, "\n")
    end

end

-- Main client function.
function run()
    conn = sr.Connection("application")
    sess = sr.Session(conn)

    subscribe = sr.Subscribe(sess)

    wrap = sr.Callback_lua(module_change_cb)
    subscribe:module_change_subscribe("ietf-interfaces", wrap);

    print("\n\n ========== READING STARTUP CONFIG: ==========\n");
    print_current_config(sess, "ietf-interfaces");

    print("\n\n ========== STARTUP CONFIG APPLIED AS RUNNING ==========\n");

    sr.global_loop()

    print("Application exit requested, exiting.\n");
end

ok,res=pcall(run)
if not ok then
    print("\nerror: ",res, "\n")
end
