open Lwt.Infix;

let rep_endpoint = ref "tcp://0.0.0.0:5555";
let rout_endpoint = ref "tcp://0.0.0.0:5556";
/* let notify_list  = ref [(("",0),[("", Int32.of_int 0)])]; */
let notify_list  = ref [];
let token_secret_key_file = ref "";
let token_secret_key = ref "";
let router_public_key = ref "";
let router_secret_key = ref "";
let log_mode = ref false;
let no_db = ref false;
let server_secret_key_file = ref "";
let server_secret_key = ref "";
let version = 1;
let identity = ref (Unix.gethostname ());
let content_format = ref "";

/* create stores in local directory by default */
let default_store_directory = "./";
let store_directory = ref default_store_directory;
let kv_json_store = ref (Database.Json.Kv.create file::(!store_directory ^ "/kv-json-store"));
let ts_complex_json_store = ref (Database.Json.Ts.create file::(!store_directory ^ "/ts-complex-json-store"));
let ts_simple_json_store = ref (Database.Json.Ts.create file::(!store_directory ^ "/ts-simple-json-store"));

let kv_text_store = ref (Database.String.Kv.create file::(!store_directory ^ "/kv-text-store"));
let kv_binary_store = ref (Database.String.Kv.create file::(!store_directory ^ "/kv-binary-store"));

let setup_logger () => {
  Lwt_log_core.default :=
    Lwt_log.channel
      template::"$(date).$(milliseconds) [$(level)] $(message)"
      close_mode::`Keep
      channel::Lwt_io.stdout
      ();
  Lwt_log_core.add_rule "*" Lwt_log_core.Error;
  Lwt_log_core.add_rule "*" Lwt_log_core.Info;
  Lwt_log_core.add_rule "*" Lwt_log_core.Debug;
};

let to_hex msg => {
  open Hex;
  String.trim (of_string msg |> hexdump_s print_chars::false);
};

let has_observed options => {
  if (Array.exists (fun (number,_) => number == 6) options) {
    true;
  } else {
    false;
  }
};

let is_observed path => {
  List.mem_assoc path !notify_list;
};

let observed_paths_exist () => {
  List.length !notify_list > 0;
};

let get_ident path => {
  List.assoc path !notify_list;
};

let time_now () => {
  Int32.of_float (Unix.time ());
};

let add_to_observe uri_path content_format ident max_age => {
  open Int32;
  let key = (uri_path, content_format);
  let expiry = (equal max_age (of_int 0)) ? max_age : add (time_now ()) max_age;
  let value = (ident, expiry);
  if (is_observed key) {
    let _ = Lwt_log_core.info_f "adding ident:%s to existing path:%s with max-age:%lu" ident uri_path max_age;
    let items = get_ident key;
    let new_items = List.cons value items;
    let filtered = List.filter (fun (key',_) => (key' != key)) !notify_list;
    notify_list := List.cons (key, new_items) filtered;
  } else {
    let _ = Lwt_log_core.info_f "adding ident:%s to new path:%s with max-age:%lu" ident uri_path max_age;
    notify_list := List.cons (key, [value]) !notify_list;
  };
};

let handle_header bits => {
  let tuple = [%bitstring
    switch bits {
    | {|code : 8 : unsigned;
        oc : 8 : unsigned;
        tkl : 16 : bigendian;
        rest : -1 : bitstring
     |} => (tkl, oc, code, rest); 
    | {|_|} => failwith "invalid header";
    };
  ];
  tuple;    
};

let handle_token bits len => {
  let tuple = [%bitstring
    switch bits {
    | {|token : len*8 : string; 
        rest : -1 : bitstring
      |} => (token, rest);
    | {|_|} => failwith "invalid token";
    };
  ];
  tuple;
};

let handle_option bits => {
  let tuple = [%bitstring
    switch bits {
    | {|number : 16 : bigendian; 
        len : 16 : bigendian;
        value: len*8: string; 
        rest : -1 : bitstring
      |} => (number, value, rest);
    | {|_|} => failwith "invalid options";
    };
  ];
  tuple;
};

let handle_options oc bits => {
  let options = Array.make oc (0,"");
  let rec handle oc bits =>
    if (oc == 0) {
      bits;
    } else {
      let (number, value, r) = handle_option bits;
      Array.set options (oc - 1) (number,value);
      let _ = Lwt_log_core.debug_f "option => %d:%s" number value;
      handle (oc - 1) r
  };
  (options, handle oc bits);
};

let create_header tkl::tkl oc::oc code::code => {
  let bits = [%bitstring 
    {|code : 8 : unsigned;
      oc : 8 : unsigned;
      tkl : 16 : bigendian         
    |}
  ];
  (bits, 32);
};

let create_option number::number value::value => {
  let byte_length = String.length value;
  let bit_length = byte_length * 8;
  let bits = [%bitstring 
    {|number : 16 : bigendian;
      byte_length : 16 : bigendian;
      value : bit_length : string
    |}
  ];
  (bits ,(bit_length+32));
};


let create_options options => {
  let count = Array.length options;
  let values = Array.map (fun (x,y) => x) options;
  let value = Bitstring.concat (Array.to_list values);
  let lengths = Array.map (fun (x,y) => y) options;
  let length = Array.fold_left (fun x y => x + y) 0 lengths;
  (value, length, count);
};

let create_ack code => {
  let (header_value, header_length) = create_header tkl::0 oc::0 code::code;
  let bits = [%bitstring {|header_value : header_length : bitstring|}];
  Bitstring.string_of_bitstring bits;
};

let create_content_format id => {
  let bits = [%bitstring {|id : 16 : bigendian|}];
  Bitstring.string_of_bitstring bits  
};

let create_ack_payload_options format::format => {
  let content_format = create_option number::12 value::format;
  create_options [|content_format|];
};

let create_ack_payload format_code payload => {
  let (options_value, options_length, options_count) = create_ack_payload_options format::(create_content_format format_code);
  let (header_value, header_length) = create_header tkl::0 oc::options_count code::69;  
  let payload_bytes = String.length payload * 8;
  let bits = [%bitstring 
    {|header_value : header_length : bitstring;
      options_value : options_length : bitstring;
      payload : payload_bytes : string
    |}
  ];
  Bitstring.string_of_bitstring bits;
};

let create_ack_observe_options format::format key::key => {
  let content_format = create_option number::12 value::format;
  let public_key = create_option number::2048 value::key;
  create_options [|content_format, public_key|];
};

let create_ack_observe public_key uuid::payload => {
  let (options_value, options_length, options_count) = create_ack_observe_options format::(create_content_format 0) key::public_key;
  let (header_value, header_length) = create_header tkl::0 oc::options_count code::69;  
  let payload_bytes = String.length payload * 8;
  let bits = [%bitstring 
    {|header_value : header_length : bitstring;
      options_value : options_length : bitstring;
      payload : payload_bytes : string
    |}
  ];
  Bitstring.string_of_bitstring bits;
};

let get_option_value options value => {
  let rec find a x i => {
    let (number,value) = a.(i);
    if (number == x) {
      value;
    } else {
      find a x (i + 1)
    };
  };
  find options value 0;
};

let get_content_format options => {
  let value = get_option_value options 12;
  let bits = Bitstring.bitstring_of_string value;
  let id = [%bitstring
    switch bits {
    | {|id : 16 : bigendian|} => id;
    | {|_|} => failwith "invalid content value";
    };
  ];
  id;
};

let get_max_age options => {
  let value = get_option_value options 14;
  let bits = Bitstring.bitstring_of_string value;
  let seconds = [%bitstring
    switch bits {
    | {|seconds : 32 : bigendian|} => seconds;
    | {|_|} => failwith "invalid max-age value";
    };
  ];
  seconds;
};


let publish path payload socket => {
  let msg = Printf.sprintf "%s %s" path payload;
  Lwt_zmq.Socket.send socket msg;
};

let expire l t => {
  open List;
  let f x =>
    switch x {
    | (k,v) => (k, filter (fun (_,t') => (t' > t) || (t' == Int32.of_int 0)) v);
    };
  filter (fun (x,y) => y != []) (map f l);
};

let diff l1 l2 => List.filter (fun x => not (List.mem x l2)) l1;

let list_uuids alist => {
  open List;  
  map (fun (x,y) => hd y) alist;    
};

let route_message alist socket payload => {
  open Lwt_zmq.Socket.Router;  
  let rec loop l => {
    switch l {
      | [] => Lwt.return_unit;
      | [(ident,expiry), ...rest] => {
          send socket (id_of_string ident) [payload] >>=
          /*Lwt_zmq.Socket.send_all socket [ident, payload] >>=*/
            fun _ => Lwt_log_core.debug_f "Routing:\n%s \nto ident:%s with expiry:%lu" (to_hex payload) ident expiry >>=
              fun _ => loop rest;
        };
      };    
  };
  loop alist;
};

let handle_expire socket => {
  if (observed_paths_exist ()) {
    open Lwt_zmq.Socket.Router;
    let new_notify_list = expire !notify_list (time_now ());
    let uuids = diff (list_uuids !notify_list) (list_uuids new_notify_list);
    notify_list := new_notify_list;
    /* send Service Unavailable */
    route_message uuids socket (create_ack 163);
  } else {
    Lwt.return_unit;
  };
};

let route tuple payload socket => {
  let (_,content_format) = tuple;
  route_message (get_ident tuple) socket (create_ack_payload content_format payload);
};

let handle_get_read_ts_complex_latest id => {
  open Common.Response;
  Json (Database.Json.Ts.Complex.read_latest !ts_complex_json_store id);
};

let handle_get_read_ts_simple_latest id => {
  open Common.Response;  
  Json (Database.Json.Ts.Simple.read_latest !ts_simple_json_store id);
};

let handle_get_read_ts_complex_earliest id => {
  open Common.Response;  
  Json (Database.Json.Ts.Complex.read_earliest !ts_complex_json_store id);
};

let handle_get_read_ts_simple_earliest id => {
  open Common.Response;  
  Json (Database.Json.Ts.Simple.read_earliest !ts_simple_json_store id);
};

let handle_get_read_ts_complex_last id n => {
  open Common.Response;  
  Json (Database.Json.Ts.Complex.read_last !ts_complex_json_store id (int_of_string n));
};

let handle_get_read_ts_simple_last id n func => {
  open Common.Response;
  open Database.Json.Ts.Simple;
  open Numeric;
  open Filter;
  let apply0 = Json (read_last !ts_simple_json_store id (int_of_string n));
  let apply1 f1 => Json (read_last_apply f1 !ts_simple_json_store id (int_of_string n));
  let apply2 f1 f2 => Json (read_last_apply2 f1 f2 !ts_simple_json_store id (int_of_string n)); 
  switch func {
  | [] => apply0;
  | ["sum"] => apply1 sum;
  | ["count"] => apply1 count;
  | ["min"] => apply1 min;
  | ["max"] => apply1 max;
  | ["mean"] => apply1 mean;
  | ["median"] => apply1 median;
  | ["sd"] => apply1 sd;
  | ["filter", s1, "equals", s2] => apply1 (equals s1 s2);
  | ["filter", s1, "contains", s2] => apply1 (contains s1 s2);
  | ["filter", s1, "equals", s2, "sum"] => apply2 (equals s1 s2) sum;
  | ["filter", s1, "contains", s2, "sum"] => apply2 (contains s1 s2) sum;
  | ["filter", s1, "equals", s2, "count"] => apply2 (equals s1 s2) count;
  | ["filter", s1, "contains", s2, "count"] => apply2 (contains s1 s2) count;
  | ["filter", s1, "equals", s2, "min"] => apply2 (equals s1 s2) min;
  | ["filter", s1, "contains", s2, "min"] => apply2 (contains s1 s2) min;
  | ["filter", s1, "equals", s2, "max"] => apply2 (equals s1 s2) max;
  | ["filter", s1, "contains", s2, "max"] => apply2 (contains s1 s2) max;
  | ["filter", s1, "equals", s2, "mean"] => apply2 (equals s1 s2) mean;
  | ["filter", s1, "contains", s2, "mean"] => apply2 (contains s1 s2) mean;
  | ["filter", s1, "equals", s2, "median"] => apply2 (equals s1 s2) median;
  | ["filter", s1, "contains", s2, "median"] => apply2 (contains s1 s2) median;
  | ["filter", s1, "equals", s2, "sd"] => apply2 (equals s1 s2) sd;
  | ["filter", s1, "contains", s2, "sd"] => apply2 (contains s1 s2) sd;
  | _ => Empty;
  };  
};

let handle_get_read_ts_complex_first id n => {
  open Common.Response;  
  Json (Database.Json.Ts.Complex.read_first !ts_complex_json_store id (int_of_string n));
};

let handle_get_read_ts_simple_first id n func => {
  open Common.Response;
  open Database.Json.Ts.Simple;
  open Numeric;
  open Filter;
  let apply0 = Json (read_first !ts_simple_json_store id (int_of_string n));
  let apply1 f => Json (read_first_apply f !ts_simple_json_store id (int_of_string n));
  let apply2 f1 f2 => Json (read_first_apply2 f1 f2 !ts_simple_json_store id (int_of_string n));
  switch func {
  | [] => apply0;
  | ["sum"] => apply1 sum;
  | ["count"] => apply1 count;
  | ["min"] => apply1 min;
  | ["max"] => apply1 max;
  | ["mean"] => apply1 mean;
  | ["median"] => apply1 median;
  | ["sd"] => apply1 sd;
  | ["filter", s1, "equals", s2] => apply1 (equals s1 s2);
  | ["filter", s1, "contains", s2] => apply1 (contains s1 s2);
  | ["filter", s1, "equals", s2, "sum"] => apply2 (equals s1 s2) sum;
  | ["filter", s1, "contains", s2, "sum"] => apply2 (contains s1 s2) sum;
  | ["filter", s1, "equals", s2, "count"] => apply2 (equals s1 s2) count;
  | ["filter", s1, "contains", s2, "count"] => apply2 (contains s1 s2) count;
  | ["filter", s1, "equals", s2, "min"] => apply2 (equals s1 s2) min;
  | ["filter", s1, "contains", s2, "min"] => apply2 (contains s1 s2) min;
  | ["filter", s1, "equals", s2, "max"] => apply2 (equals s1 s2) max;
  | ["filter", s1, "contains", s2, "max"] => apply2 (contains s1 s2) max;
  | ["filter", s1, "equals", s2, "mean"] => apply2 (equals s1 s2) mean;
  | ["filter", s1, "contains", s2, "mean"] => apply2 (contains s1 s2) mean;
  | ["filter", s1, "equals", s2, "median"] => apply2 (equals s1 s2) median;
  | ["filter", s1, "contains", s2, "median"] => apply2 (contains s1 s2) median;
  | ["filter", s1, "equals", s2, "sd"] => apply2 (equals s1 s2) sd;
  | ["filter", s1, "contains", s2, "sd"] => apply2 (contains s1 s2) sd;
  | _ => Empty;
  };    
};

let handle_get_read_ts_complex_since id t => {
  open Common.Response;  
  Json (Database.Json.Ts.Complex.read_since !ts_complex_json_store id (int_of_string t));
};

let handle_get_read_ts_simple_since id t func => {
  open Common.Response;
  open Database.Json.Ts.Simple;
  open Numeric;
  open Filter;
  let apply0 = Json (read_since !ts_simple_json_store id (int_of_string t));
  let apply1 f => Json (read_since_apply f !ts_simple_json_store id (int_of_string t));
  let apply2 f1 f2 => Json (read_since_apply2 f1 f2 !ts_simple_json_store id (int_of_string t));
  switch func {
  | [] => apply0;
  | ["sum"] => apply1 sum;
  | ["count"] => apply1 count;
  | ["min"] => apply1 min;
  | ["max"] => apply1 max;
  | ["mean"] => apply1 mean;
  | ["median"] => apply1 median;
  | ["sd"] => apply1 sd;
  | ["filter", s1, "equals", s2] => apply1 (equals s1 s2);
  | ["filter", s1, "contains", s2] => apply1 (contains s1 s2);
  | ["filter", s1, "equals", s2, "sum"] => apply2 (equals s1 s2) sum;
  | ["filter", s1, "contains", s2, "sum"] => apply2 (contains s1 s2) sum;
  | ["filter", s1, "equals", s2, "count"] => apply2 (equals s1 s2) count;
  | ["filter", s1, "contains", s2, "count"] => apply2 (equals s1 s2) count; 
  | ["filter", s1, "equals", s2, "min"] => apply2 (equals s1 s2) min;
  | ["filter", s1, "contains", s2, "min"] => apply2 (contains s1 s2) min; 
  | ["filter", s1, "equals", s2, "max"] => apply2 (equals s1 s2) max;
  | ["filter", s1, "contains", s2, "max"] => apply2 (contains s1 s2) max;
  | ["filter", s1, "equals", s2, "mean"] => apply2 (equals s1 s2) mean;
  | ["filter", s1, "contains", s2, "mean"] => apply2 (contains s1 s2) mean;
  | ["filter", s1, "equals", s2, "median"] => apply2 (equals s1 s2) median;
  | ["filter", s1, "contains", s2, "median"] => apply2 (contains s1 s2) median;
  | ["filter", s1, "equals", s2, "sd"] => apply2 (equals s1 s2) sd;
  | ["filter", s1, "contains", s2, "sd"] => apply2 (contains s1 s2) sd;
  | _ => Empty;
  };    
  
};

let handle_get_read_ts_complex_range id t1 t2 => {
  open Common.Response;  
  Json (Database.Json.Ts.Complex.read_range !ts_complex_json_store id (int_of_string t1) (int_of_string t2));
};

let handle_get_read_ts_simple_range id t1 t2 func => {
  open Common.Response;  
  open Database.Json.Ts.Simple;
  open Numeric;
  open Filter;
  let apply0 = Json (read_range !ts_simple_json_store id (int_of_string t1) (int_of_string t2));
  let apply1 f => Json (read_range_apply f !ts_simple_json_store id (int_of_string t1) (int_of_string t2));
  let apply2 f1 f2 => Json (read_range_apply2 f1 f2 !ts_simple_json_store id (int_of_string t1) (int_of_string t2));
  switch func {
  | [] => apply0;
  | ["sum"] => apply1 sum;
  | ["count"] => apply1 count;
  | ["min"] => apply1 min;
  | ["max"] => apply1 max;
  | ["mean"] => apply1 mean;
  | ["median"] => apply1 median;
  | ["sd"] => apply1 sd;
  | ["filter", s1, "equals", s2] => apply1 (equals s1 s2);
  | ["filter", s1, "contains", s2] => apply1 (contains s1 s2);
  | ["filter", s1, "equals", s2, "sum"] => apply2 (equals s1 s2) sum;
  | ["filter", s1, "contains", s2, "sum"] => apply2 (contains s1 s2) sum;
  | ["filter", s1, "equals", s2, "count"] => apply2 (equals s1 s2) count;
  | ["filter", s1, "contains", s2, "count"] => apply2 (contains s1 s2) count;
  | ["filter", s1, "equals", s2, "min"] => apply2 (equals s1 s2) min;
  | ["filter", s1, "contains", s2, "min"] => apply2 (contains s1 s2) min;
  | ["filter", s1, "equals", s2, "max"] => apply2 (equals s1 s2) max;
  | ["filter", s1, "contains", s2, "max"] => apply2 (contains s1 s2) max;
  | ["filter", s1, "equals", s2, "mean"] => apply2 (equals s1 s2) mean;
  | ["filter", s1, "contains", s2, "mean"] => apply2 (contains s1 s2) mean;
  | ["filter", s1, "equals", s2, "median"] => apply2 (equals s1 s2) median;
  | ["filter", s1, "contains", s2, "median"] => apply2 (contains s1 s2) median;
  | ["filter", s1, "equals", s2, "sd"] => apply2 (equals s1 s2) sd;
  | ["filter", s1, "contains", s2, "sd"] => apply2 (contains s1 s2) sd;
  | _ => Empty;
  }; 
  
};

let handle_get_read_ts uri_path => {
  open List;
  open Common.Response;  
  let path_list = String.split_on_char '/' uri_path;
  switch path_list {
  | ["", "ts", "blob", id, "latest"] => handle_get_read_ts_complex_latest id;
  | ["", "ts", id, "latest"] => handle_get_read_ts_simple_latest id;
  | ["", "ts", "blob", id, "earliest"] => handle_get_read_ts_complex_earliest id;
  | ["", "ts", id, "earliest"] => handle_get_read_ts_simple_earliest id;
  | ["", "ts", "blob", id, "last", n] => handle_get_read_ts_complex_last id n;
  | ["", "ts", id, "last", n, ...func] => handle_get_read_ts_simple_last id n func;
  | ["", "ts", "blob", id, "first", n] => handle_get_read_ts_complex_first id n;
  | ["", "ts", id, "first", n, ...func] => handle_get_read_ts_simple_first id n func;
  | ["", "ts", "blob", id, "since", t] => handle_get_read_ts_complex_since id t;
  | ["", "ts", id, "since", t, ...func] => handle_get_read_ts_simple_since id t func;
  | ["", "ts", "blob", id, "range", t1, t2] => handle_get_read_ts_complex_range id t1 t2;
  | ["", "ts", id, "range", t1, t2, ...func] => handle_get_read_ts_simple_range id t1 t2 func;
  | _ => Empty;
  };
};


let get_key mode uri_path => {
  let path_list = String.split_on_char '/' uri_path;
  switch path_list {
  | ["", mode, key] => Some key; 
  | _ => None;
  };
};

let get_mode uri_path => {
  Str.first_chars uri_path 4;
};

let handle_get_read_kv_json uri_path => {
  open Common.Response;  
  let key = get_key "kv" uri_path;
  switch key {
  | Some k => Json (Database.Json.Kv.read !kv_json_store k);
  | _ => Empty;
  };
};

let handle_get_read_kv_binary uri_path => {
  open Common.Response;  
  let key = get_key "kv" uri_path;
  switch key {
  | Some k => Binary (Database.String.Kv.read !kv_binary_store k);
  | _ => Empty;
  };
};

let handle_get_read_kv_text uri_path => {
  open Common.Response;  
  let key = get_key "kv" uri_path;
  switch key {
  | Some k => Text (Database.String.Kv.read !kv_text_store k);
  | _ => Empty;
  };
}; 

let handle_read_database content_format uri_path => {
  open Common.Ack;
  open Common.Response;
  let mode = get_mode uri_path;
  let result = switch (mode, content_format) {
  | ("/kv/", 50) => handle_get_read_kv_json uri_path;
  | ("/ts/", 50) => handle_get_read_ts uri_path;
  | ("/kv/", 42) => handle_get_read_kv_binary uri_path;
  | ("/kv/", 0) => handle_get_read_kv_text uri_path;
  | _ => Empty;
  };
  switch result {
  | Json json => json >>= fun json' => 
        Lwt.return (Payload content_format (Ezjsonm.to_string json'));
  | Text text => text >>= fun text' =>
        Lwt.return (Payload content_format text');
  | Binary binary => binary >>= fun binary' =>
        Lwt.return (Payload content_format binary');
  | Empty => Lwt.return (Code 128);
  };
};

let handle_read_hypercat () => {
  open Common.Ack;
  Hypercat.get_cat () |> Ezjsonm.to_string |>
    fun s => (Payload 50 s) |> Lwt.return;
};

let handle_get_read content_format uri_path => {
  switch uri_path {
  | "/cat" => handle_read_hypercat ();
  | _ => handle_read_database content_format uri_path; 
  };
};

let to_json payload => {
  open Ezjsonm;
  let parsed = try (Some (from_string payload)) {
  | Parse_error _ => None;
  };
  parsed;
};


let handle_post_write_ts_complex ::timestamp=None key payload => {
  let json = to_json payload;
  switch json {
  | Some value => Some (Database.Json.Ts.Complex.write !ts_complex_json_store timestamp key value);
  | None => None;
  };  
};

let handle_post_write_ts_simple ::timestamp=None key payload => {
  open Database.Json.Ts.Simple;
  let json = to_json payload;
  switch json {
  | Some value => {
      if (is_valid value) {
        Some (write !ts_simple_json_store timestamp key value);
      } else None;
    };
  | None => None;
  };  
};

let handle_post_write_ts uri_path payload => {
  open List;
  let path_list = String.split_on_char '/' uri_path;
  switch path_list {
  | ["", "ts", "blob", key] => 
    handle_post_write_ts_complex key payload;
  | ["", "ts", "blob", key, "at", ts] => 
    handle_post_write_ts_complex timestamp::(Some (int_of_string ts)) key payload;
  | ["", "ts", key] => 
    handle_post_write_ts_simple key payload;
  | ["", "ts", key, "at", ts] => 
    handle_post_write_ts_simple timestamp::(Some (int_of_string ts)) key payload;
  | _ => None;
  };
};

let handle_post_write_kv_json uri_path payload => {
  let key = get_key "kv" uri_path;
  let json = to_json payload;
  switch (key,json) {
  | (Some k, Some v) => Some (Database.Json.Kv.write !kv_json_store k v);
  | _ => None;
  };
};

let handle_post_write_kv_binary uri_path payload => {
  let key = get_key "kv" uri_path;
  switch key {
  | Some k => Some (Database.String.Kv.write !kv_binary_store k payload);
  | _ => None;
  };  
};

let handle_post_write_kv_text uri_path payload => {
  let key = get_key "kv" uri_path;
  switch key {
  | Some k => Some (Database.String.Kv.write !kv_text_store k payload);
  | _ => None;
  };
};
  

let handle_write_database content_format uri_path payload => {
  open Common.Ack;
  open Ezjsonm;
  let mode = get_mode uri_path;
  let result = switch (mode, content_format) {
  | ("/kv/", 50) => handle_post_write_kv_json uri_path payload;
  | ("/ts/", 50) => handle_post_write_ts uri_path payload;  
  | ("/kv/", 42) => handle_post_write_kv_binary uri_path payload;
  | ("/kv/", 0) => handle_post_write_kv_text uri_path payload;
  | _ => None;
  };
  switch result {
  | Some promise => promise >>= fun () => Lwt.return (Code 65);
  | None => Lwt.return (Code 128);
  };
};

let handle_write_hypercat payload => {
  open Common.Ack;
  let json = to_json payload;
  switch json {
  | Some json => {
      switch (Hypercat.update_cat json) {
        | Ok => (Code 65)
        | Error n => (Code n)
        } |> Lwt.return;
    };
  | None => Lwt.return (Code 128);
  };
};

let handle_post_write content_format uri_path payload => {
  switch uri_path {
  | "/cat" => handle_write_hypercat payload;
  | _ => handle_write_database content_format uri_path payload; 
  };
};

let ack kind => {
  open Common.Ack;
  switch kind {
  | Code n => create_ack n;
  | Payload format data => create_ack_payload format data;
  | Observe key uuid => create_ack_observe key uuid;
  } |> Lwt.return;
};

let create_uuid () => {
  Uuidm.v4_gen (Random.State.make_self_init ()) () |> Uuidm.to_string;
};

let is_valid_token token path meth => {
  switch !token_secret_key {
  | "" => true;
  | _ => Token.is_valid token !token_secret_key ["path = " ^ path, "method = " ^ meth, "target = " ^ !identity];
  };
};

let handle_content_format options => {
  let content_format = get_content_format options;
  let _ = Lwt_log_core.debug_f "content_format => %d" content_format;
  content_format;
};

let handle_max_age options => {
  let max_age = get_max_age options;
  let _ = Lwt_log_core.debug_f "max_age => %lu" max_age;
  max_age;
};

let handle_get options token => {
  open Common.Ack;
  let content_format = handle_content_format options;
  let uri_path = get_option_value options 11;
  if ((is_valid_token token uri_path "GET") == false) {
    ack (Code 129)
  } else if (has_observed options) {
    let max_age = handle_max_age options;  
    let uuid = create_uuid ();
    add_to_observe uri_path content_format uuid max_age;
    ack (Observe !router_public_key uuid);
  } else {
    handle_get_read content_format uri_path >>= ack;
  };
};


let handle_post options token payload rout_soc => {
  open Common.Ack;
  let content_format = handle_content_format options;
  let uri_path = get_option_value options 11;
  let tuple = (uri_path, content_format);
  if ((is_valid_token token uri_path "POST") == false) {
    ack (Code 129);
  } else if (is_observed tuple) {
      handle_post_write content_format uri_path payload >>=
        fun resp => {
          /* we dont want to route bad requests */
          if (resp != (Code 128)) {
            route tuple payload rout_soc >>= fun () => ack resp;
          } else {
            ack resp;
          };
      };
  } else {
    handle_post_write content_format uri_path payload >>= ack;
  };
};

let handle_msg msg rout_soc => {
  handle_expire rout_soc >>=
    fun () =>
      Lwt_log_core.debug_f "Received:\n%s" (to_hex msg) >>=
        fun () => {
          let r0 = Bitstring.bitstring_of_string msg;
          let (tkl, oc, code, r1) = handle_header r0;
          let (token, r2) = handle_token r1 tkl;
          let (options,r3) = handle_options oc r2;
          let payload = Bitstring.string_of_bitstring r3;
          switch code {
          | 1 => handle_get options token;
          | 2 => handle_post options token payload rout_soc;
          | _ => failwith "invalid code";
          };
        };  
};

let server_test_nodb rep_soc rout_soc => {
  open Common.Ack;
  let rec loop () => {
    Lwt_zmq.Socket.recv rep_soc >>=
      fun msg =>
        ack (Code 65) >>=
          fun resp =>
            Lwt_zmq.Socket.send rep_soc resp >>=
              fun () =>
                Lwt_log_core.debug_f "Sending:\n%s" (to_hex resp) >>=
                  fun () => loop ();
  };
  loop ();
};

let server rep_soc rout_soc => {
  let rec loop () => {
    Lwt_zmq.Socket.recv rep_soc >>=
      fun msg =>
        handle_msg msg rout_soc >>=
          fun resp =>
            Lwt_zmq.Socket.send rep_soc resp >>=
              fun () =>
                Lwt_log_core.debug_f "Sending:\n%s" (to_hex resp) >>=
                  fun () => loop ();
  };
  loop ();
};

let setup_rep_socket endpoint ctx kind secret => {
  open ZMQ.Socket;
  let soc = ZMQ.Socket.create ctx kind;
  ZMQ.Socket.set_receive_high_water_mark soc 1;
  set_linger_period soc 0;
  set_curve_server soc true;
  set_curve_secretkey soc secret; 
  bind soc endpoint;
  Lwt_zmq.Socket.of_socket soc;
};

let setup_rout_socket endpoint ctx kind secret => {
  open ZMQ.Socket;
  let soc = ZMQ.Socket.create ctx kind;
  ZMQ.Socket.set_receive_high_water_mark soc 1;
  set_linger_period soc 0;
  set_curve_server soc true;
  set_curve_secretkey soc secret; 
  bind soc endpoint;
  Lwt_zmq.Socket.of_socket soc;
};

let close_socket lwt_soc => {
  let soc = Lwt_zmq.Socket.to_socket lwt_soc;
  ZMQ.Socket.close soc;
};


/* test key: uf4XGHI7[fLoe&aG1tU83[ptpezyQMVIHh)J=zB1 */


let parse_cmdline () => {
  let usage = "usage: " ^ Sys.argv.(0) ^ " [--debug] [--secret-key string]";
  let speclist = [
    ("--request-endpoint", Arg.Set_string rep_endpoint, ": to set the request/reply endpoint"),
    ("--router-endpoint", Arg.Set_string rout_endpoint, ": to set the router/dealer endpoint"),
    ("--enable-logging", Arg.Set log_mode, ": turn debug mode on"),
    ("--secret-key-file", Arg.Set_string server_secret_key_file, ": to set the curve secret key"),
    ("--token-key-file", Arg.Set_string token_secret_key_file, ": to set the token secret key"),
    ("--identity", Arg.Set_string identity, ": to set the server identity"),
    ("--store-dir", Arg.Set_string store_directory, ": to set the location for the database files"),
    ("--no-db", Arg.Set no_db, ": test without using database"),
  ];
  Arg.parse speclist (fun x => raise (Arg.Bad ("Bad argument : " ^ x))) usage;
};

let setup_router_keys () => {
  let (public_key,private_key) = ZMQ.Curve.keypair ();
  router_secret_key := private_key;
  router_public_key := public_key;
};

/* some issues running these threads so disabled */
let monitor_connections ctx rep_soc rout_soc => {
  let () = Connections.monitor ctx rout_soc;
  let () = Connections.monitor ctx rep_soc;
};

/* support overriding location of stores */
let create_stores_again () => {
  kv_json_store := Database.Json.Kv.create file::(!store_directory ^ "/kv-json-store");
  ts_complex_json_store := Database.Json.Ts.create file::(!store_directory ^ "/ts-complex-json-store");
  ts_simple_json_store := Database.Json.Ts.create file::(!store_directory ^ "/ts-simple-json-store");
  kv_text_store := Database.String.Kv.create file::(!store_directory ^ "/kv-text-store");
  kv_binary_store := Database.String.Kv.create file::(!store_directory ^ "/kv-binary-store");
};

let data_from_file file => {
  Fpath.v file |>
    Bos.OS.File.read |>
      fun result =>
        switch result {
        | Rresult.Error _ => failwith "failed to access file";
        | Rresult.Ok key => key;
        };
};

let set_server_key file => {
  server_secret_key := (data_from_file file);
};

let set_token_key file => {
  if (file != "") { 
    token_secret_key := (data_from_file file);
  };
};

let report_error e rep_soc => {
  let msg = Printexc.to_string e;
  let stack = Printexc.get_backtrace ();
  let _ = Lwt_log_core.error_f "Opps: %s%s" msg stack;
  let _ = ack (Common.Ack.Code 128) >>= fun resp => Lwt_zmq.Socket.send rep_soc resp;
};

let the_server rep_soc rout_soc => {
  !no_db ? server_test_nodb rep_soc rout_soc : server rep_soc rout_soc;
};

let rec run_server rep_soc rout_soc => {
  let _ = Lwt_log_core.info "Ready";   
  try (Lwt_main.run {the_server rep_soc rout_soc}) {
    | e => report_error e rep_soc;
  };
  run_server rep_soc rout_soc;
};

let terminate_server ctx rep_soc rout_soc => {
  close_socket rout_soc;
  close_socket rep_soc;
  ZMQ.Context.terminate ctx;
};

let setup_server () => {
  parse_cmdline ();
  !log_mode ? setup_logger () : ();
  setup_router_keys ();
  set_server_key !server_secret_key_file;
  set_token_key !token_secret_key_file;
  (!store_directory != default_store_directory) ? create_stores_again () : ();
  let ctx = ZMQ.Context.create ();
  let rep_soc = setup_rep_socket !rep_endpoint ctx ZMQ.Socket.rep !server_secret_key;
  let rout_soc = setup_rout_socket !rout_endpoint ctx ZMQ.Socket.router !router_secret_key;
  run_server rep_soc rout_soc |> fun () => terminate_server ctx rep_soc rout_soc;
};

setup_server ();

