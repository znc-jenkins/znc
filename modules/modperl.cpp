#ifdef HAVE_PERL
#include "main.h"
#include "User.h"
#include "Nick.h"
#include "Modules.h"
#include "Chan.h"
#include "FileUtils.h"
#include "Csocket.h"

// perl stuff
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#define NICK( a ) a.GetNickMask()
#define CHAN( a ) a.GetName()
#define ZNCEvalCB "ZNC::COREEval"
#define ZNCCallFuncCB "ZNC::CORECallFunc"
#define ZNCCallTimerCB "ZNC::CORECallTimer"
#define ZNCCallSockCB "ZNC::CORECallSock"
#define ZNCSOCK ":::ZncSock:::"

// TODO use PutStatus instead of PutModule
class PString : public CString 
{
public:
	enum EType
	{
		STRING,
		INT,
		UINT,
		NUM,
		BOOL
	};
	
	PString() : CString() { m_eType = STRING; }
	PString( const char* c ) : CString(c) { m_eType = STRING; }
	PString( const CString& s ) : CString(s) { m_eType = STRING; }
	PString( int i ) : CString( PString::ToString( i ) ) { m_eType = INT; }
	PString( u_int i ) : CString( PString::ToString( i ) ) { m_eType = UINT; }
	PString( long i ) : CString( PString::ToString( i ) ) { m_eType = INT; }
	PString( u_long i ) : CString( PString::ToString( i ) ) { m_eType = UINT; }
	PString( long long i ) : CString( PString::ToString( (long long)i ) ) { m_eType = INT; }
	PString( unsigned long long i ) : CString( PString::ToString( i ) ) { m_eType = UINT; }
	PString( double i ) : CString( PString::ToString( i ) ) { m_eType = NUM; }
	PString( bool b ) : CString( ( b ? "1" : "0" ) ) { m_eType = BOOL; }

	virtual ~PString() {}

	
	EType GetType() const { return( m_eType ); }
	void SetType( EType e ) { m_eType = e; }
	
	SV * GetSV( bool bMakeMortal = true ) const
	{
		SV *pSV = NULL;
		switch( GetType() )
		{
			case NUM:
				pSV = newSVnv( ToDouble() );
				break;
			case INT:
				pSV = newSViv( ToLongLong() );
				break;
			case UINT:
			case BOOL:
				pSV = newSVuv( ToULongLong() );
				break;
			case STRING:
			default:
				pSV = newSVpv( data(), length() );
				break;
		}

		if ( bMakeMortal )
			pSV = sv_2mortal( pSV );

		return( pSV );
	}

private:
	
	EType	m_eType;
};


class CPerlHash : public map< CString, PString >
{
public:
	
	HV *GetHash()
	{
		HV *pHash = newHV();
		sv_2mortal( (SV *) pHash );
		for( CPerlHash::iterator it = this->begin(); it != this->end(); it++ )
		{
			SV *pSV = it->second.GetSV( false );
			hv_store( pHash, it->first.c_str(), it->first.length(), pSV, 0);
		}

		return( pHash );
	}
};

typedef vector< PString > VPString;

class CModPerl;
static CModPerl *g_ModPerl = NULL;

class CPerlSock : public Csock
{
public:
	CPerlSock() : Csock()
	{
		m_iParentFD = -1;
		SetSockName( ZNCSOCK );
	}
	CPerlSock( const CS_STRING & sHost, int iPort, int iTimeout = 60 )
		: Csock( sHost, iPort, iTimeout ) 
	{
		m_iParentFD = -1;
		SetSockName( ZNCSOCK );
	}


// # OnSockDestroy( $sockhandle )
	virtual ~CPerlSock();

	virtual Csock *GetSockObj( const CS_STRING & sHostname, int iPort );

	void SetParentFD( int iFD ) { m_iParentFD = iFD; }
	void SetUsername( const CString & sUsername ) { m_sUsername = sUsername; }
	void SetModuleName( const CString & sModuleName ) { m_sModuleName = sModuleName; }

	const CString &  GetUsername() { return( m_sUsername ); }
	const CString &  GetModuleName() { return( m_sModuleName ); }

// # OnConnect( $sockhandle, $parentsockhandle )
	virtual void Connected();
// # OnConnectionFrom( $sockhandle, $remotehost, $remoteport )
	virtual bool ConnectionFrom( const CS_STRING & sHost, int iPort );
// # OnError( $sockhandle, $errno )
	virtual void SockError( int iErrno );
// # OnConnectionRefused( $sockhandle )
	virtual void ConnectionRefused();
// # OnTimeout( $sockhandle )
	virtual void Timeout();
// # OnDisconnect( $sockhandle )
	virtual void Disconnected();
// # OnData( $sockhandle, $bytes, $length )
	virtual void ReadData( const char *data, int len );
// # OnReadLine( $sockhandle, $line )
	virtual void ReadLine( const CS_STRING & sLine );


private:
	CString		m_sModuleName;
	CString		m_sUsername;	// NEED these so we can send the signal to the right guy
	int			m_iParentFD;
	VPString	m_vArgs;

	void SetupArgs()
	{
		m_vArgs.clear();
		m_vArgs.push_back( m_sModuleName );
		m_vArgs.push_back( GetRSock() );
	}

	void AddArg( const PString & sArg )
	{
		m_vArgs.push_back( sArg );
	}

	int CallBack( const PString & sFuncName );
};

class CPerlTimer : public CTimer 
{
public:
	CPerlTimer( CModule* pModule, unsigned int uInterval, unsigned int uCycles, const CString& sLabel, const CString& sDescription ) 
		: CTimer( pModule, uInterval, uCycles, sLabel, sDescription) {}

	virtual ~CPerlTimer() {}

	// TODO possibly need to check that the userspace is correct when this goes global

	void SetFuncName( const CString & sFuncName ) { m_sFuncName = sFuncName; }
	void SetUserName( const CString & sUserName ) { m_sUserName = sUserName; }
	void SetModuleName( const CString & sModuleName ) { m_sModuleName = sModuleName; }

protected:
	virtual void RunJob();

	CString		m_sFuncName;
	CString		m_sUserName;
	CString		m_sModuleName;
};

class CModPerl : public CModule 
{
public:
	MODCONSTRUCTOR( CModPerl ) 
	{
		g_ModPerl = this;
		m_pPerl = NULL;
	}

	virtual ~CModPerl() 
	{
		DestroyAllSocks();
		if ( m_pPerl )
		{
			CBNone( "Shutdown" );
			PerlInterpShutdown();
		}
		g_ModPerl = NULL;
	}

	void PerlInterpShutdown()
	{
		perl_destruct( m_pPerl );
		perl_free( m_pPerl );
		m_pPerl = NULL;
	}

	void SetupZNCScript()
	{
		CString sModule = m_pUser->FindModPath( "modperl.pm" );
		if ( !sModule.empty() )
		{
			CString sBuffer, sScript;
			CFile cFile( sModule );
			if ( ( cFile.Exists() ) && ( cFile.Open( O_RDONLY ) ) )
			{
				while( cFile.ReadLine( sBuffer ) )
					sScript += sBuffer;	
				cFile.Close();

				eval_pv( sScript.c_str(), FALSE );
			}
		}
	}

	void DumpError( const CString & sError )
	{
		CString sTmp = sError;
		for( CString::size_type a = 0; a < sTmp.size(); a++ )
		{
			if ( isspace( sTmp[a] ) )
				sTmp[a] = ' ';
		}
		PutModule( sTmp );
	}

	TSocketManager<Csock> * GetSockManager() { return( m_pManager ); }
	void DestroyAllSocks();

	CUser * GetUser() { return( m_pUser ); }

	virtual bool OnLoad( const CString & sArgs );
	virtual bool OnBoot() { return( ( CBNone( "OnBoot" ) == CONTINUE ) ); }
	virtual void OnUserAttached() {  CBNone( "OnUserAttached" ); }
	virtual void OnUserDetached() {  CBNone( "OnUserDetached" ); }
	virtual void OnIRCDisconnected() {  CBNone( "OnIRCDisconnected" ); }
	virtual void OnIRCConnected() {  CBNone( "OnIRCConnected" ); }

	virtual EModRet OnDCCUserSend(const CNick& RemoteNick, unsigned long uLongIP, unsigned short uPort, 
		const CString& sFile, unsigned long uFileSize);

	virtual void OnOp(const CNick& OpNick, const CNick& Nick, CChan& Channel, bool bNoChange)
	{
		CBFour( "OnOp", NICK( OpNick ), NICK( Nick ), CHAN( Channel ), bNoChange );
	}
	virtual void OnDeop(const CNick& OpNick, const CNick& Nick, CChan& Channel, bool bNoChange)
	{
		CBFour( "OnDeop", NICK( OpNick ), NICK( Nick ), CHAN( Channel ), bNoChange );
	}
	virtual void OnVoice(const CNick& OpNick, const CNick& Nick, CChan& Channel, bool bNoChange)
	{
		CBFour( "OnVoice", NICK( OpNick ), NICK( Nick ), CHAN( Channel ), bNoChange );
	}
	virtual void OnDevoice(const CNick& OpNick, const CNick& Nick, CChan& Channel, bool bNoChange)
	{
		CBFour( "OnDevoice", NICK( OpNick ), NICK( Nick ), CHAN( Channel ), bNoChange );
	}
	virtual void OnRawMode(const CNick& Nick, CChan& Channel, const CString& sModes, const CString& sArgs)
	{
		CBFour( "OnRawMode", NICK( Nick ), CHAN( Channel ), sModes, sArgs  );
	}
	virtual EModRet OnUserRaw(CString& sLine) { return( CBSingle( "OnUserRaw", sLine ) ); }
	virtual EModRet OnRaw(CString& sLine) { return( CBSingle( "OnRaw", sLine ) ); }

	virtual void OnModCommand(const CString& sCommand) 
	{ 
		if ( CBSingle( "OnModCommand", sCommand ) == 0 )
			Eval( sCommand );
	}
	virtual void OnModNotice(const CString& sMessage) { CBSingle( "OnModNotice", sMessage ); }
	virtual void OnModCTCP(const CString& sMessage) { CBSingle( "OnModCTCP", sMessage ); }

	virtual void OnQuit(const CNick& Nick, const CString& sMessage, const vector<CChan*>& vChans)
	{
		VPString vsArgs;
		vsArgs.push_back( Nick.GetNickMask() );
		vsArgs.push_back( sMessage );
		for( vector<CChan*>::size_type a = 0; a < vChans.size(); a++ )
			vsArgs.push_back( vChans[a]->GetName() );

		CallBack( "OnQuit", vsArgs );
	}

	virtual void OnNick(const CNick& Nick, const CString& sNewNick, const vector<CChan*>& vChans)
	{
		VPString vsArgs;
		vsArgs.push_back( Nick.GetNickMask() );
		vsArgs.push_back( sNewNick );
		for( vector<CChan*>::size_type a = 0; a < vChans.size(); a++ )
			vsArgs.push_back( vChans[a]->GetName() );

		CallBack( "OnNick", vsArgs );
	}

	virtual void OnKick(const CNick& Nick, const CString& sOpNick, CChan& Channel, const CString& sMessage)
	{
		CBFour( "OnKick", NICK( Nick ), sOpNick, CHAN( Channel ), sMessage );
	}

	virtual void OnJoin(const CNick& Nick, CChan& Channel) { CBDouble( "OnJoin", NICK( Nick ), CHAN( Channel ) ); }
	virtual void OnPart(const CNick& Nick, CChan& Channel) { CBDouble( "OnPart", NICK( Nick ), CHAN( Channel ) ); }

	virtual EModRet OnUserCTCPReply(const CNick& Nick, CString& sMessage) 
	{ 
		return CBDouble( "OnUserCTCPReply", NICK( Nick ), sMessage ); 
	}
	virtual EModRet OnCTCPReply(const CNick& Nick, CString& sMessage)
	{
		return CBDouble( "OnCTCPReply", NICK( Nick ), sMessage ); 
	}
	virtual EModRet OnUserCTCP(const CString& sTarget, CString& sMessage)
	{
		return CBDouble( "OnUserCTCP", sTarget, sMessage ); 
	}
	virtual EModRet OnPrivCTCP(const CNick& Nick, CString& sMessage)
	{
		return CBDouble( "OnPrivCTCP", NICK( Nick ), sMessage ); 
	}
	virtual EModRet OnChanCTCP(const CNick& Nick, CChan& Channel, CString& sMessage)
	{
		return CBTriple( "OnChanCTCP", NICK( Nick ), CHAN( Channel ), sMessage );
	}
	virtual EModRet OnUserMsg(const CString& sTarget, CString& sMessage)
	{
		return CBDouble( "OnUserMsg", sTarget, sMessage );
	}
	virtual EModRet OnPrivMsg(const CNick& Nick, CString& sMessage)
	{
		return CBDouble( "OnPrivMsg", NICK( Nick ), sMessage );
	}

	virtual EModRet OnChanMsg( const CNick& Nick, CChan & Channel, CString & sMessage )
	{
		return( CBTriple( "OnChanMsg", NICK( Nick ), CHAN( Channel ), sMessage ) );
	}
	virtual EModRet OnUserNotice(const CString& sTarget, CString& sMessage)
	{
		return CBDouble( "OnUserNotice", sTarget, sMessage );
	}
	virtual EModRet OnPrivNotice(const CNick& Nick, CString& sMessage)
	{
		return CBDouble( "OnPrivNotice", NICK( Nick ), sMessage );
	}
	virtual EModRet OnChanNotice(const CNick& Nick, CChan& Channel, CString& sMessage)
	{
		return( CBTriple( "OnChanNotice", NICK( Nick ), CHAN( Channel ), sMessage ) );
	}

	enum ECBTYPES
	{
		CB_LOCAL 	= 1,
		CB_ONHOOK 	= 2,
		CB_TIMER 	= 3,
		CB_SOCK		= 4
	};

	EModRet CallBack( const PString & sHookName, const VPString & vsArgs, 
			ECBTYPES eCBType = CB_ONHOOK, const PString & sUsername = "" );

	EModRet CBNone( const PString & sHookName )
	{
		VPString vsArgs;
		return( CallBack( sHookName, vsArgs ) );
	}

	template <class A>
	inline EModRet CBSingle( const PString & sHookName, const A & a )
	{
		VPString vsArgs;
		vsArgs.push_back( a );
		return( CallBack( sHookName, vsArgs ) );
	}
	template <class A, class B>
	inline EModRet CBDouble( const PString & sHookName, const A & a, const B & b )
	{
		VPString vsArgs;
		vsArgs.push_back( a );
		vsArgs.push_back( b );
		return( CallBack( sHookName, vsArgs ) );
	}
	template <class A, class B, class C>
	inline EModRet CBTriple( const PString & sHookName, const A & a, const B & b, const C & c )
	{
		VPString vsArgs;
		vsArgs.push_back( a );
		vsArgs.push_back( b );
		vsArgs.push_back( c );
		return( CallBack( sHookName, vsArgs ) );
	}
	template <class A, class B, class C, class D>
	inline EModRet CBFour( const PString & sHookName, const A & a, const B & b, const C & c, const D & d )
	{
		VPString vsArgs;
		vsArgs.push_back( a );
		vsArgs.push_back( b );
		vsArgs.push_back( c );
		vsArgs.push_back( d );
		return( CallBack( sHookName, vsArgs ) );
	}

	bool Eval( const CString & sScript, const CString & sFuncName = ZNCEvalCB );

	virtual EModRet OnStatusCommand( const CString& sLine )
	{
		CString sCommand = sLine.Token( 0 );

		if ( ( sCommand == "loadmod" ) || ( sCommand == "unloadmod" ) || ( sCommand == "reloadmod" ) )
		{
			CString sModule = sLine.Token( 1 );
			// TODO make this sexy, use wldcmp or Right( 3 )
			if ( sModule.find( ".pm" ) != CString::npos )
			{
				if ( sCommand == "loadmod" )
					LoadPerlMod( sModule );
				else if ( sCommand == "unloadmod" )
					UnLoadPerlMod( sModule );
				else
					PutModule( "Perl modules can not be reloaded one at a time, you have to reload the interpreter to pick up code changes (reloadmod modperl)" );
				return( HALT );
			}
		}
		return( CONTINUE );
	}

	void LoadPerlMod( const CString & sModule );
	void UnLoadPerlMod( const CString & sModule );

private:
	PerlInterpreter	*m_pPerl;

};

MODULEDEFS( CModPerl )



//////////////////////////////// PERL GUTS //////////////////////////////

XS(XS_ZNC_COREAddTimer)
{
	dXSARGS;
	if ( items != 5 )
		Perl_croak( aTHX_ "Usage: COREAddTimer( modname, funcname, description, interval, cycles )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sModName = (char *)SvPV(ST(0),PL_na);
			CString sFuncName = (char *)SvPV(ST(1),PL_na);
			CString sDesc = (char *)SvPV(ST(2),PL_na);
			u_int iInterval = (u_int)SvUV(ST(3));
			u_int iCycles = (u_int)SvUV(ST(4));
			CString sUserName = g_ModPerl->GetUser()->GetUserName();
			CString sLabel = sUserName + sModName + sFuncName;
			CPerlTimer *pTimer = new CPerlTimer( g_ModPerl, iInterval, iCycles, sLabel, sDesc );
			pTimer->SetFuncName( sFuncName );
			pTimer->SetUserName( sUserName );
			pTimer->SetModuleName( sModName );
			g_ModPerl->AddTimer( pTimer );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_CORERemTimer)
{
	dXSARGS;
	if ( items != 2 )
		Perl_croak( aTHX_ "Usage: CORERemTimer( modname, funcname )" );
	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sModName = (char *)SvPV(ST(0),PL_na);
			CString sFuncName = (char *)SvPV(ST(1),PL_na);
			CString sUserName = g_ModPerl->GetUser()->GetUserName();
			CString sLabel = sUserName + sModName + sFuncName;
			CTimer *pTimer = g_ModPerl->FindTimer( sLabel );
			if ( pTimer )
				pTimer->Stop();
			else
				g_ModPerl->PutModule( "Unable to find Timer!" );
		}
		PUTBACK;
	}
}
XS(XS_ZNC_COREPuts)
{
	dXSARGS;
	if ( items != 2 )
		Perl_croak( aTHX_ "Usage: COREPuts( sWHich, sLine )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sWhich = (char *)SvPV(ST(0),PL_na);
			CString sLine = (char *)SvPV(ST(1),PL_na);

			if ( sWhich == "IRC" )
				g_ModPerl->PutIRC( sLine );
			else if ( sWhich == "Status" )
				g_ModPerl->PutStatus( sLine );
			else if ( sWhich == "User" )
				g_ModPerl->PutUser( sLine );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_LoadMod)
{
	dXSARGS;
	if ( items != 1 )
		Perl_croak( aTHX_ "Usage: LoadMod( module )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sModule = (char *)SvPV(ST(0),PL_na);
			g_ModPerl->LoadPerlMod( sModule );
		}
		PUTBACK;
	}
}
	
XS(XS_ZNC_UnLoadMod)
{
	dXSARGS;
	if ( items != 1 )
		Perl_croak( aTHX_ "Usage: UnLoadMod( module )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sModule = (char *)SvPV(ST(0),PL_na);
			g_ModPerl->UnLoadPerlMod( sModule );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_COREPutModule)
{
	dXSARGS;
	if ( items != 4 )
		Perl_croak( aTHX_ "Usage: COREPutModule( sWhich sLine, sIdent, sHost )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sWhich = (char *)SvPV(ST(0),PL_na);
			CString sLine = (char *)SvPV(ST(1),PL_na);
			CString sIdent = (char *)SvPV(ST(2),PL_na);
			CString sHost = (char *)SvPV(ST(3),PL_na);
			if ( sWhich == "Module" )
				g_ModPerl->PutModule( sLine, sIdent, sHost );
			else
				g_ModPerl->PutModNotice( sLine, sIdent, sHost );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_GetNicks)
{
	dXSARGS;
	if ( items != 1 )
		Perl_croak( aTHX_ "Usage: GetNicks( sChan )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CString sChan = (char *)SvPV(ST(0),PL_na);
			CUser * pUser = g_ModPerl->GetUser();
			CChan * pChan = pUser->FindChan( sChan );
			if ( !pChan )
				XSRETURN( 0 );
			
			const map< CString,CNick* > & mscNicks = pChan->GetNicks();

			for( map< CString,CNick* >::const_iterator it = mscNicks.begin(); it != mscNicks.end(); it++ )
			{
				CNick & cNick = *(it->second);
				CPerlHash cHash;
				cHash["Nick"] = cNick.GetNick();
				cHash["Ident"] = cNick.GetIdent();
				cHash["Host"] = cNick.GetHost();
				cHash["Perms"] = cNick.GetPermStr();
				XPUSHs( newRV_noinc( (SV*)cHash.GetHash() ) );
			}
		}
		PUTBACK;
	}
}

XS(XS_ZNC_GetString)
{
	dXSARGS;
	
	if ( items != 1 )
		Perl_croak( aTHX_ "Usage: GetString( sName )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			CUser * pUser = g_ModPerl->GetUser();
			PString sReturn;
			CString sName = (char *)SvPV(ST(0),PL_na);

			if( sName == "UserName" ) sReturn = pUser->GetUserName();
			else if ( sName == "Nick" ) sReturn = pUser->GetNick();
			else if ( sName == "AltNick" ) sReturn = pUser->GetAltNick();
			else if ( sName == "Ident" ) sReturn = pUser->GetIdent();
			else if ( sName == "RealName" ) sReturn = pUser->GetRealName();
			else if ( sName == "VHost" ) sReturn = pUser->GetVHost();
			else if ( sName == "Pass" ) sReturn = pUser->GetPass();
			else if ( sName == "CurPath" ) sReturn = pUser->GetCurPath();
			else if ( sName == "DLPath" ) sReturn = pUser->GetDLPath();
			else if ( sName == "ModPath" ) sReturn = pUser->GetModPath();
			else if ( sName == "HomePath" ) sReturn = pUser->GetHomePath();
			else if ( sName == "DataPath" ) sReturn = pUser->GetDataPath();
			else if ( sName == "StatusPrefix" ) sReturn = pUser->GetStatusPrefix();
			else if ( sName == "DefaultChanModes" ) sReturn = pUser->GetDefaultChanModes();
			else if ( sName == "IRCServer" ) sReturn = pUser->GetIRCServer();
			else
				XSRETURN( 0 );

			XPUSHs( sReturn.GetSV() );
		}
		PUTBACK;
	}
}

////////////// Perl SOCKET guts /////////////
#define MANAGER g_ModPerl->GetSockManager()
XS(XS_ZNC_WriteSock)
{
	dXSARGS;
	if ( items != 3 )
		Perl_croak( aTHX_ "Usage: ZNC::WriteSock( sockhandle, bytes, len )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			PString sReturn = false;
			int iSockFD = SvIV(ST(0));
			u_int iLen = SvUV(ST(2));
			if ( iLen > 0 )
			{
				PString sData;
				sData.append( SvPV( ST(1), iLen ), iLen );
				CPerlSock *pSock = (CPerlSock *)MANAGER->FindSockByFD( iSockFD );
				if ( ( pSock ) && ( pSock->GetSockName() == ZNCSOCK ) )
					sReturn = pSock->Write( sData.data(), sData.length() );
			}
			XPUSHs( sReturn.GetSV() );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_CloseSock)
{
	dXSARGS;
	if ( items != 1 )
		Perl_croak( aTHX_ "Usage: ZNC::CloseSock( sockhandle )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			PString sReturn = false;
			int iSockFD = SvIV(ST(0));
			CPerlSock *pSock = (CPerlSock *)MANAGER->FindSockByFD( iSockFD );
			if ( ( pSock ) && ( pSock->GetSockName() == ZNCSOCK ) )
				pSock->Close();
		}
		PUTBACK;
	}
}

XS(XS_ZNC_COREConnect)
{
	dXSARGS;
	if ( items != 6 )
		Perl_croak( aTHX_ "Usage: ZNC::COREConnect( $modname, $host, $port, $timeout, $bEnableReadline, $bUseSSL )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			PString sReturn = -1;
			PString sModuleName = (char *)SvPV(ST(0),PL_na);
			PString sHostname = (char *)SvPV(ST(1),PL_na);
			int iPort = SvIV(ST(2));
			u_int iTimeout = SvUV(ST(3));
			u_int iEnableReadline = SvUV(ST(4));
			u_int iUseSSL = SvUV(ST(5));
			CPerlSock *pSock = new CPerlSock( sHostname, iPort, iTimeout );
			pSock->SetSockName( ZNCSOCK );
			pSock->SetUsername( g_ModPerl->GetUser()->GetUserName() );
			pSock->SetModuleName( sModuleName );
			if ( iEnableReadline )
				pSock->EnableReadLine();

			if ( MANAGER->Connect( sHostname, iPort, ZNCSOCK, iTimeout, ( iUseSSL != 0 ), "", pSock ) )
				sReturn = pSock->GetRSock();

			XPUSHs( sReturn.GetSV() );
		}
		PUTBACK;
	}
}

XS(XS_ZNC_COREListen)
{
	dXSARGS;
	if ( items != 5 )
		Perl_croak( aTHX_ "Usage: ZNC::COREListen( $modname, $port, $bindhost, $bEnableReadline, $bUseSSL )" );

	SP -= items;
	ax = (SP - PL_stack_base) + 1 ;
	{
		if ( g_ModPerl )
		{
			PString sReturn = -1;
			PString sModuleName = (char *)SvPV(ST(0),PL_na);
			int iPort = SvIV(ST(1));
			PString sHostname = (char *)SvPV(ST(2),PL_na);
			u_int iEnableReadline = SvUV(ST(3));
			u_int iUseSSL = SvUV(ST(4));

			CPerlSock *pSock = new CPerlSock();
			pSock->SetSockName( ZNCSOCK );
			pSock->SetUsername( g_ModPerl->GetUser()->GetUserName() );
			pSock->SetModuleName( sModuleName );

			if ( iEnableReadline )
				pSock->EnableReadLine();

			bool bContinue = true;
			if ( iUseSSL != 0 )
			{
				if ( CFile::Exists( g_ModPerl->GetUser()->GetPemLocation() ) )
					pSock->SetPemLocation( g_ModPerl->GetUser()->GetPemLocation() );
				else
				{
					PutModule( "PEM File does not exist! (looking for " + g_ModPerl->GetUser()->GetPemLocation() + ")" );
					bContinue = false;
				}
			}

			if ( bContinue )
			{
				if ( MANAGER->ListenHost( iPort, ZNCSOCK, sHostname, ( iUseSSL != 0 ), SOMAXCONN, pSock ) )
					sReturn = pSock->GetRSock();
			}
			else
				sReturn = -1;

			XPUSHs( sReturn.GetSV() );
		
		}
		PUTBACK;
	}
}

/////////// supporting functions from within module

bool CModPerl::Eval( const CString & sScript, const CString & sFuncName )
{
	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK( SP );
	XPUSHs( sv_2mortal( newSVpv( sScript.c_str(), sScript.length() ) ) );
	PUTBACK;

	SPAGAIN;

	call_pv( sFuncName.c_str(), G_EVAL|G_KEEPERR|G_VOID|G_DISCARD );

	bool bReturn = true;

	if ( SvTRUE( ERRSV ) ) 
	{ 
		DumpError( SvPV( ERRSV, PL_na) );
		bReturn = false;
	}
	PUTBACK;
	FREETMPS;
	LEAVE;

	return( bReturn );
}

CModPerl::EModRet CModPerl::CallBack( const PString & sHookName, const VPString & vsArgs, 
		ECBTYPES eCBType, const PString & sUsername )
{
	if ( !m_pPerl )
		return( CONTINUE );
	
	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK( SP );

	CString sFuncToCall;
	if ( eCBType == CB_LOCAL )
		sFuncToCall = sHookName;
	else
	{
		if ( sUsername.empty() )
			XPUSHs( PString( m_pUser->GetUserName() ).GetSV() );
		else
			XPUSHs( sUsername.GetSV() );
		XPUSHs( sHookName.GetSV() );
		if ( eCBType == CB_ONHOOK )
			sFuncToCall = ZNCCallFuncCB;
		else if ( eCBType == CB_TIMER )
			sFuncToCall = ZNCCallTimerCB;
		else
			sFuncToCall = ZNCCallSockCB;
	}
	for( VPString::size_type a = 0; a < vsArgs.size(); a++ )
		XPUSHs( vsArgs[a].GetSV() );

	PUTBACK;

	int iCount = call_pv( sFuncToCall.c_str(), G_EVAL|G_SCALAR );

	SPAGAIN;
	int iRet = CONTINUE;

	if ( SvTRUE( ERRSV ) ) 
	{ 
		CString sError = SvPV( ERRSV, PL_na);
		DumpError( sHookName + ": " + sError );

		if ( eCBType == CB_TIMER )
			iRet = HALT;
	} else
	{
		if ( iCount == 1 )
			iRet = POPi;
	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return( (CModPerl::EModRet)iRet );
}

////////////////////// Events ///////////////////

// special case this, required for perl modules that are dynamic
EXTERN_C void boot_DynaLoader (pTHX_ CV* cv);

bool CModPerl::OnLoad( const CString & sArgs ) 
{
	m_pPerl = perl_alloc();
	perl_construct( m_pPerl );
	const char *pArgv[] = { "", "-e", "0", "-T", "-w" };

	if ( perl_parse( m_pPerl, NULL, 2, (char **)pArgv, (char **)NULL ) != 0 )
	{
		perl_free( m_pPerl );
		m_pPerl = NULL;
		return( false );
	}

#ifdef PERL_EXIT_DESTRUCT_END
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
#endif /* PERL_EXIT_DESTRUCT_END */

	char *file = __FILE__;

	/* system functions */
	newXS( "DynaLoader::boot_DynaLoader", boot_DynaLoader, (char *)file );
	newXS( "ZNC::COREPutModule", XS_ZNC_COREPutModule, (char *)file );
	newXS( "ZNC::COREAddTimer", XS_ZNC_COREAddTimer, (char *)file );
	newXS( "ZNC::CORERemTimer", XS_ZNC_CORERemTimer, (char *)file );
	newXS( "ZNC::COREPuts", XS_ZNC_COREPuts, (char *)file );
	newXS( "ZNC::COREConnect", XS_ZNC_COREConnect, (char *)file );
	newXS( "ZNC::COREListen", XS_ZNC_COREListen, (char *)file );

	/* user functions */
	newXS( "ZNC::GetNicks", XS_ZNC_GetNicks, (char *)file );
	newXS( "ZNC::GetString", XS_ZNC_GetString, (char *)file );
	newXS( "ZNC::LoadMod", XS_ZNC_LoadMod, (char *)file );
	newXS( "ZNC::UnLoadMod", XS_ZNC_UnLoadMod, (char *)file );
	newXS( "ZNC::WriteSock", XS_ZNC_WriteSock, (char *)file );
	newXS( "ZNC::CloseSock", XS_ZNC_CloseSock, (char *)file );
	
	// this sets up the eval CB that we call from here on out. this way we can grab the error produced
	SetupZNCScript();

	HV *pZNCSpace = get_hv ( "ZNC::", TRUE);

	if ( !pZNCSpace )
		return( false );

	newCONSTSUB( pZNCSpace, "CONTINUE", newSViv( CONTINUE ) );
	newCONSTSUB( pZNCSpace, "HALT", newSViv( HALT ) );
	newCONSTSUB( pZNCSpace, "HALTMODS", newSViv( HALTMODS ) );
	newCONSTSUB( pZNCSpace, "HALTCORE", newSViv( HALTCORE ) );

	for( u_int a = 0; a < 255; a++ )
	{
		CString sModule = sArgs.Token( a );
		if ( sModule.empty() )
			break;

		LoadPerlMod( sModule );
	}

	return( true );
}

void CModPerl::LoadPerlMod( const CString & sModule )
{
	CString sModPath = m_pUser->FindModPath( sModule );
	if ( sModPath.empty() )
		PutModule( "No such module " + sModule );
	else
	{
		PutModule( "Using " + sModPath );
		Eval( "ZNC::CORELoadMod( '" + m_pUser->GetUserName() + "', '" + sModPath + "');" );
	}
}

void CModPerl::DestroyAllSocks()
{
	for( u_int a = 0; a < m_pManager->size(); a++ )
	{
		if ( (*m_pManager)[a]->GetSockName() == ZNCSOCK )
		{
			m_pManager->DelSock( a-- );
		}
	}
}
void CModPerl::UnLoadPerlMod( const CString & sModule )
{
	DestroyAllSocks();
	Eval( "ZNC::COREUnLoadMod( '" + m_pUser->GetUserName() + "', '" + sModule + "');" );
}


CModPerl::EModRet CModPerl::OnDCCUserSend(const CNick& RemoteNick, unsigned long uLongIP, unsigned short uPort, 
		const CString& sFile, unsigned long uFileSize)
{
	VPString vsArgs;
	vsArgs.push_back( NICK( RemoteNick ) );
	vsArgs.push_back( uLongIP );
	vsArgs.push_back( uPort );
	vsArgs.push_back( sFile );
	
	return( CallBack( "OnDCCUserSend", vsArgs ) );
}

void CPerlTimer::RunJob()
{
	VPString vArgs;
	vArgs.push_back( m_sModuleName );
	if ( ((CModPerl *)m_pModule)->CallBack( m_sFuncName, vArgs, CModPerl::CB_TIMER ) != CModPerl::CONTINUE )
		Stop();
}

/////////////////////////// CPerlSock stuff ////////////////////
#define SOCKCB( a ) if ( CallBack( a ) != CModPerl::CONTINUE ) { Close(); }
int CPerlSock::CallBack( const PString & sFuncName )
{
	// TODO need to lookup by our username and set the correct user into g_ModPerl
	return( g_ModPerl->CallBack( sFuncName, m_vArgs, CModPerl::CB_SOCK, m_sUsername ) );
}

// # OnConnect( $sockhandle, $parentsockhandle )
void CPerlSock::Connected()
{
	if ( GetType() == INBOUND )
	{
		m_vArgs.clear();
		m_vArgs.push_back( m_sModuleName );
		m_vArgs.push_back( m_iParentFD );
		m_vArgs.push_back( GetRSock() );
		SOCKCB( "OnNewSock" );
	}
	SetupArgs();
	if ( GetType() == INBOUND )
		AddArg( m_iParentFD );

	SOCKCB( "OnConnect" )
}

// # OnConnectionFrom( $sockhandle, $remotehost, $remoteport )
bool CPerlSock::ConnectionFrom( const CS_STRING & sHost, int iPort )
{
	SetupArgs();
	AddArg( sHost );
	AddArg( iPort );

	// special case here
	if ( CallBack( "OnConnectionFrom" ) != CModPerl::CONTINUE )
		return( false );

	return( true );
}


// # OnError( $sockhandle, $errno )
void CPerlSock::SockError( int iErrno )
{
	SetupArgs();
	AddArg( iErrno );
	SOCKCB( "OnError" )
}

// # OnConnectionRefused( $sockhandle )
void CPerlSock::ConnectionRefused()
{
	SetupArgs();
	SOCKCB( "OnConnectionRefused" )
}

// # OnTimeout( $sockhandle )
void CPerlSock::Timeout()
{
	SetupArgs();
	SOCKCB( "OnTimeout" )
}

// # OnDisconnect( $sockhandle )
void CPerlSock::Disconnected()
{
	SetupArgs();
	SOCKCB( "OnDisconnect" );
}
// # OnData( $sockhandle, $bytes, $length )
void CPerlSock::ReadData( const char *data, int len )
{
	SetupArgs();
	PString sData;
	sData.append( data, len );
	AddArg( sData );
	AddArg( len );
	SOCKCB( "OnData" )
}
// # OnReadLine( $sockhandle, $line )
void CPerlSock::ReadLine( const CS_STRING & sLine )
{
	SetupArgs();
	AddArg( sLine );
	SOCKCB( "OnReadLine" );
}

// # OnSockDestroy( $sockhandle )
CPerlSock::~CPerlSock()
{
	SetupArgs();
	CallBack( "OnSockDestroy" );
}

Csock *CPerlSock::GetSockObj( const CS_STRING & sHostname, int iPort )
{
	CPerlSock *p = new CPerlSock( sHostname, iPort );
	p->SetParentFD( GetRSock() );
	p->SetUsername( m_sUsername );
	p->SetModuleName( m_sModuleName );
	p->SetSockName( ZNCSOCK );
	return( p );
}

#endif /* HAVE_PERL */








