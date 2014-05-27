#include <bts/wallet/wallet.hpp>
#include <bts/wallet/wallet_db.hpp>
#include <bts/wallet/config.hpp>
#include <fc/thread/thread.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/filesystem.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>

namespace bts { namespace wallet {

   namespace detail {

      class wallet_impl
      {
         public:
             wallet_db          _wallet_db;
             chain_database_ptr _blockchain;
             fc::path           _data_directory;
             fc::path           _current_wallet_path;
             fc::time_point     _scheduled_lock_time;
             fc::future<void>   _wallet_relocker_done;
             fc::sha512         _wallet_password;
      };

   } // detail 

   wallet::wallet( chain_database_ptr blockchain )
   :my( new detail::wallet_impl() )
   {
      my->_blockchain = blockchain;
   }

   wallet::~wallet()
   {
      close();
   }

   void           wallet::set_data_directory( const fc::path& data_dir )
   {
      my->_data_directory = data_dir;
   }

   fc::path       wallet::get_data_directory()const
   {
      return my->_data_directory;
   }

   void wallet::create( const std::string& wallet_name, 
                        const std::string& password,
                        const std::string& brainkey  )
   { try {
      create_file( fc::absolute(my->_data_directory) / wallet_name, password, brainkey ); 
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${name}' in ${data_dir}", 
                            ("wallet_name",wallet_name)("data_dir",fc::absolute(my->_data_directory)) ) }


   void wallet::create_file( const fc::path& wallet_file_path,
                        const std::string& password,
                        const std::string& brainkey  )
   { try {
      FC_ASSERT( !fc::exists( wallet_file_path ) );
      FC_ASSERT( password.size() > 8 );
      my->_wallet_db.open( wallet_file_path );
      
      auto password_hash = fc::sha512::hash( password.c_str(), password.size() );

      master_key new_master_key;
      if( brainkey.size() )
      {
         auto base = fc::sha512::hash( brainkey.c_str(), brainkey.size() );

         /* strengthen the key a bit */
         for( uint32_t i = 0; i < 100ll*1000ll; ++i )
            base = fc::sha512::hash( base );

         new_master_key.encrypt_key( password_hash, extended_private_key( base ) );
      }
      else
      {
         extended_private_key epk( fc::ecc::private_key::generate() );
         new_master_key.encrypt_key( password_hash, epk );
      }

      my->_wallet_db.store_record( wallet_master_key_record( new_master_key ) );

      my->_wallet_db.close();
      my->_wallet_db.open( wallet_file_path );

      FC_ASSERT( my->_wallet_db.wallet_master_key.valid() );

   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${wallet_file_path}'", 
                            ("wallet_file_path",wallet_file_path) ) }


   void wallet::open( const std::string& wallet_name )
   { try {
      open_file( get_data_directory() / wallet_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("wallet_name",wallet_name) ) }
   

   void wallet::open_file( const fc::path& wallet_filename )
   { 
      try {
         close();
         my->_wallet_db.open( wallet_filename );
         my->_current_wallet_path = wallet_filename;
      } FC_RETHROW_EXCEPTIONS( warn, 
             "Unable to open wallet ${filename}", 
             ("filename",wallet_filename) ) 
   }

   void wallet::close()
   {
      my->_wallet_db.close();
      if( my->_wallet_relocker_done.valid() )
      {
         lock();
         my->_wallet_relocker_done.cancel();
         if( my->_wallet_relocker_done.ready() ) 
           my->_wallet_relocker_done.wait();
      }
   }

   std::string wallet::get_wallet_name()const
   {
      return my->_current_wallet_path.filename().generic_string();
   }

   fc::path wallet::get_wallet_filename()const
   {
      return my->_current_wallet_path;
   }

   bool wallet::is_open()const
   {
      return my->_wallet_db.is_open();
   }

   void wallet::export_to_json( const fc::path& export_file_name ) const
   {
      my->_wallet_db.export_to_json( export_file_name );
   }

   void wallet::unlock( const fc::microseconds& timeout, const std::string& password )
   { try {
      FC_ASSERT( password.size() > BTS_MIN_PASSWORD_LENGTH ) 
      FC_ASSERT( timeout >= fc::seconds(1) );
      FC_ASSERT( my->_wallet_db.wallet_master_key.valid() );

      my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );
      if( !my->_wallet_db.wallet_master_key->validate_password( my->_wallet_password ) )
      {
         lock();
         FC_ASSERT( !"Invalid Password" );
      }

      if( timeout == fc::microseconds::maximum() )
      {
         my->_scheduled_lock_time = fc::time_point::maximum();
      }
      else
      {
         my->_scheduled_lock_time = fc::time_point::now() + timeout;
         if( !my->_wallet_relocker_done.valid() || my->_wallet_relocker_done.ready() )
         {
           my->_wallet_relocker_done = fc::async([this](){
             while( !my->_wallet_relocker_done.canceled() )
             {
               if (fc::time_point::now() > my->_scheduled_lock_time)
               {
                 lock();
                 return;
               }
               fc::usleep(fc::microseconds(200000));
             }
           });
         }
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("timeout_sec", timeout.count()/1000000 ) ) }

   void wallet::lock()
   {
      my->_wallet_password     = fc::sha512();
      my->_scheduled_lock_time = fc::time_point();
      my->_wallet_relocker_done.cancel();
   }
   void wallet::change_passphrase( const std::string& new_passphrase )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( !"TODO - Implement CHange Passphrase" );
   }

   bool wallet::is_unlocked()const
   {
      return !wallet::is_locked();
   }   

   bool wallet::is_locked()const
   {
      return my->_wallet_password == fc::sha512();
   }

   fc::time_point wallet::unlocked_until()const
   {
      return my->_scheduled_lock_time;
   }

   public_key_type  wallet::create_account( const std::string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( !current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password );
      auto new_pub_key  = new_priv_key.get_public_key();

      my->_wallet_db.add_account( account_name, new_pub_key );

      return new_pub_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   /**
    *  Creates a new account from an existing foreign private key
    */
   void wallet::import_account( const std::string& account_name, 
                                const std::string& wif_private_key )
   { try {
      auto current_account = my->_wallet_db.lookup_account( account_name );

      auto imported_public_key = import_wif_private_key( wif_private_key, std::string() );
      if( current_account.valid() )
      {
         FC_ASSERT( current_account->account_address == address( imported_public_key ) );
         import_wif_private_key( wif_private_key, account_name );
      }
      else
      {
         my->_wallet_db.add_account( account_name, imported_public_key );
         import_wif_private_key( wif_private_key, account_name );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   /**
    *  Creates a new private key under the specified account. This key
    *  will not be valid for sending TITAN transactions to, but will
    *  be able to receive payments directly.
    */
   address  wallet::get_new_address( const std::string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( !current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password, 
                                                          current_account->account_address );
      return new_priv_key.get_public_key();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   /**
    *  A contact is an account for which this wallet does not have the private
    *  key.  
    *
    *  @param account_name - the name the account is known by to this wallet
    *  @param key - the public key that will be used for sending TITAN transactions
    *               to the account.
    */
   void  wallet::add_contact( const std::string& account_name, 
                              const public_key_type& key )
   { try {
      FC_ASSERT( is_open() );
      auto current_account = my->_wallet_db.lookup_account( account_name );
      if( current_account.valid() )
      {
         FC_ASSERT( current_account->account_address == address(key),
                    "Account with ${name} already exists", ("name",account_name) );
      }
      else
      {
         my->_wallet_db.add_account( account_name, key );
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name)("public_key",key) ) }


   void  wallet::rename_account( const std::string& old_account_name, 
                                 const std::string& new_account_name )
   { try {
      FC_ASSERT( is_open() );
      auto old_account = my->_wallet_db.lookup_account( old_account_name );
      FC_ASSERT( old_account.valid() );

      auto new_account = my->_wallet_db.lookup_account( new_account_name );
      FC_ASSERT( !new_account.valid() );

      my->_wallet_db.rename_account( old_account_name, new_account_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", 
                ("old_account_name",old_account_name)
                ("new_account_name",new_account_name) ) }


   public_key_type  wallet::import_private_key( const private_key_type& key, 
                                                const std::string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto current_account = my->_wallet_db.lookup_account( account_name );

      if( account_name != std::string() )
         FC_ASSERT( current_account.valid() );

      auto pub_key = key.get_public_key();
      address key_address(pub_key);
      auto current_key_record = my->_wallet_db.lookup_key( key_address );
      if( current_key_record.valid() )
      {
         FC_ASSERT( current_key_record->account_address == current_account->account_address );
         return current_key_record->public_key;
      }

      key_data new_key_data;
      if( current_account.valid() )
         new_key_data.account_address = current_account->account_address;
      new_key_data.encrypt_private_key( my->_wallet_password, key );

      my->_wallet_db.store_key( new_key_data );

      return pub_key;

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   public_key_type wallet::import_wif_private_key( const std::string& wif_key, 
                                        const std::string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto wif_bytes = fc::from_base58(wif_key);
      auto key_bytes = std::vector<char>(wif_bytes.begin() + 1, wif_bytes.end() - 4);
      auto key = fc::variant(key_bytes).as<fc::ecc::private_key>();
      auto check = fc::sha256::hash( wif_bytes.data(), wif_bytes.size() - 4 );

      if( 0 == memcmp( (char*)&check, wif_bytes.data() + wif_bytes.size() - 4, 4 ) )
         return import_private_key( key, account_name );
      
      FC_ASSERT( !"Error parsing WIF private key" );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


} } // bts::wallet

